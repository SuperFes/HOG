// How much room a baked world takes in shard files, measured on a random sample of the shards of
// a smaller planet. Run: HOG_BENCH_DIR=/path [HOG_BENCH_SAMPLE=512] [HOG_BENCH_THREADS=8]
//   hog_globe_tests "[.store-size]"
#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace hog::globe;

namespace {

// Column-bricks a side of a shard, and the brick layers kept under and over the ground's.
constexpr int shard_bricks = 16;
constexpr int below = 6;
constexpr int above = 8;
constexpr double earth_lapse = 0.0065;

// What the generator's temperature and pore water are, from only the column's ground and surface
// temperature and the voxel's own height and distance.
struct Baseline {
    const Planet* planet;
    double geotherm;
    double lapse;

    [[nodiscard]] std::uint16_t temperature(const Voxel& voxel, double surface, double ground,
                                            double radius) const {
        const double cold = surface - (lapse * std::max(0.0, ground - planet->radius));
        double kelvin = 0.0;
        if (voxel.distance < 0) {
            kelvin = cold + (geotherm * std::max(0.0, ground - radius));
        } else if (voxel.liquid_amount > 0) {
            kelvin = cold;
        } else {
            kelvin = surface - (lapse * std::max(0.0, radius - planet->radius));
        }
        return temperature_in_eighths(kelvin);
    }
    [[nodiscard]] std::uint16_t water(const Voxel& voxel, double radius) const {
        if (radius >= planet->radius) {
            return 0;
        }
        return voxel.distance < 0 ? amount_of(properties(voxel.solid).porosity) : amount_of(1.0);
    }
};

struct Streams {
    std::vector<std::byte> a, b, c, d, e; // bricks as they are; exact baseline; coarse baseline; and the last two grouped
    // Where the bytes go: each field of every voxel in the window as its own plane, and the
    // distance again predicted from its neighbours.
    std::vector<std::byte> p_dist, p_pred, p_norm, p_rest, p_height, p_height_raw;
    std::vector<std::byte> r20, r50, rn20, q20n; // narrow band: only within 1.5 voxels of the surface // the shard as one block: distance rounded, predicted from its neighbours, differences only
    std::vector<std::byte> q10, q20, q50; // bricks with the distance rounded to 1, 2, 5 cm, normals left out
    std::vector<std::vector<std::byte>> heights; // the ground at other precisions and spacings
    std::size_t leaves_d = 0, leaves_e = 0, uniform_leaves_d = 0, uniform_leaves_e = 0;
    std::size_t bricks = 0;
    std::size_t same_a = 0, same_b = 0, same_c = 0;     // bricks folded into the one before
    std::size_t uniform_a = 0, uniform_b = 0, uniform_c = 0;
    std::size_t voxels = 0, off_exact = 0, off_coarse = 0; // temperature residuals not zero
    std::size_t water_off = 0;
};

// One column-brick's bricks into a stream: a brick equal to the last is a byte and a count.
class Runs {
public:
    void add(hog::ByteWriter& out, const Brick& brick, std::size_t& same, std::size_t& uniform) {
        if (have_ && brick == last_) {
            ++count_;
            ++same;
            return;
        }
        flush(out);
        last_ = brick;
        have_ = true;
        count_ = 1;
        if (brick.is_uniform()) {
            ++uniform;
        }
    }
    void flush(hog::ByteWriter& out) {
        if (!have_) {
            return;
        }
        out.write(static_cast<std::uint8_t>(count_));
        last_.write(out);
        have_ = false;
    }

private:
    Brick last_;
    bool have_ = false;
    unsigned count_ = 0;
};

// A shard's bricks as a grid 16 x 16 columns by `height` layers, each an index into `bricks`; any
// box of identical bricks is one node, at any size, and the rest split along their longest side.
struct Grid {
    int height = 0;
    std::vector<Brick> bricks;
    std::vector<std::size_t> at; // column (bj, bi), then layer
    [[nodiscard]] std::size_t& ref(int bi, int bj, int layer) {
        return at[(static_cast<std::size_t>(layer) * shard_bricks * shard_bricks) +
                  (static_cast<std::size_t>(bj) * shard_bricks) + static_cast<std::size_t>(bi)];
    }
};

struct Box {
    int x0, x1, y0, y1, z0, z1;
};

bool all_same(Grid& grid, const Box& box) {
    const std::size_t first = grid.ref(box.x0, box.y0, box.z0);
    for (int z = box.z0; z < box.z1; ++z) {
        for (int y = box.y0; y < box.y1; ++y) {
            for (int x = box.x0; x < box.x1; ++x) {
                const std::size_t other = grid.ref(x, y, z);
                if (other != first && !(grid.bricks[other] == grid.bricks[first])) {
                    return false;
                }
            }
        }
    }
    return true;
}

void write_tree(hog::ByteWriter& out, Grid& grid, const Box& box, std::size_t& leaves,
                std::size_t& solid_leaves) {
    if (all_same(grid, box)) {
        out.write(static_cast<std::uint8_t>(1));
        const Brick& brick = grid.bricks[grid.ref(box.x0, box.y0, box.z0)];
        brick.write(out);
        ++leaves;
        solid_leaves += brick.is_uniform() ? 1U : 0U;
        return;
    }
    out.write(static_cast<std::uint8_t>(0));
    const int dx = box.x1 - box.x0;
    const int dy = box.y1 - box.y0;
    const int dz = box.z1 - box.z0;
    Box low = box;
    Box high = box;
    if (dx >= dy && dx >= dz) {
        low.x1 = high.x0 = box.x0 + (dx / 2);
    } else if (dy >= dz) {
        low.y1 = high.y0 = box.y0 + (dy / 2);
    } else {
        low.z1 = high.z0 = box.z0 + (dz / 2);
    }
    write_tree(out, grid, low, leaves, solid_leaves);
    write_tree(out, grid, high, leaves, solid_leaves);
}

Streams build_shard(const Generator& generator, const Baseline& base, int face, int si, int sj,
                    bool heights_only) {
    const Layers& layers = generator.layers();
    const double lod = voxel_lod(generator);
    const double band = distance_band(layers);
    hog::ByteWriter a, b, c, q10, q20, q50, q20n;
    Streams streams;
    constexpr int side = shard_bricks * brick_side;
    std::vector<std::int32_t> ground_mm(static_cast<std::size_t>(side * side), 0);
    // The bricks of the exact and coarse baselines by column and layer, for grouping.
    struct Column { int first = 0; int last = 0; std::vector<Brick> b, c; std::vector<std::int16_t> d20, d50, n20; };
    std::vector<Column> kept(static_cast<std::size_t>(shard_bricks * shard_bricks));
    for (int bj = 0; bj < shard_bricks; ++bj) {
        for (int bi = 0; bi < shard_bricks; ++bi) {
            const Cell corner{.level = static_cast<std::uint8_t>(layers.level),
                              .face = static_cast<std::uint8_t>(face),
                              .i = static_cast<std::uint32_t>(((si * shard_bricks) + bi) * brick_side),
                              .j = static_cast<std::uint32_t>(((sj * shard_bricks) + bj) * brick_side)};
            const std::vector<Generator::Column> columns = generator.columns(corner, brick_side, lod);
            const std::size_t middle = (brick_side / 2 * brick_side) + (brick_side / 2);
            for (std::size_t k = 0; k < columns.size(); ++k) {
                const int cx = (bi * brick_side) + static_cast<int>(k % brick_side);
                const int cy = (bj * brick_side) + static_cast<int>(k / brick_side);
                ground_mm[static_cast<std::size_t>((cy * side) + cx)] =
                    static_cast<std::int32_t>(std::lround((columns[k].ground() - generator.planet().radius) * 1000.0));
            }
            const double coarse_ground = columns[middle].ground();
            const int first = std::max(0, (layers.layer_at(coarse_ground) / brick_side) - below);
            const int last = std::min(layers.count / brick_side - 1,
                                      (layers.layer_at(coarse_ground) / brick_side) + above);
            // Each column's surface temperature by its latitude, as the generator takes it.
            std::vector<double> surface(columns.size());
            for (std::size_t k = 0; k < columns.size(); ++k) {
                const Cell cell{.level = corner.level,
                                .face = corner.face,
                                .i = corner.i + static_cast<std::uint32_t>(k % brick_side),
                                .j = corner.j + static_cast<std::uint32_t>(k / brick_side)};
                surface[k] = temperature_at(*base.planet, std::asin(center(cell).z));
            }
            if (heights_only) {
                continue;
            }
            Runs run_a, run_b, run_c, run_q10, run_q20, run_q50, run_q20n;
            std::size_t same_q = 0, uniform_q = 0;
            kept[static_cast<std::size_t>((bj * shard_bricks) + bi)].first = first;
            kept[static_cast<std::size_t>((bj * shard_bricks) + bi)].last = last;
            a.write(static_cast<std::uint16_t>(first));
            a.write(static_cast<std::uint8_t>(last - first + 1));
            b.write(static_cast<std::uint16_t>(first));
            b.write(static_cast<std::uint8_t>(last - first + 1));
            c.write(static_cast<std::uint16_t>(first));
            c.write(static_cast<std::uint8_t>(last - first + 1));
            for (int layer = first; layer <= last; ++layer) {
                std::array<Voxel, brick_voxels> va{};
                auto vb = va;
                auto vc = va;
                for (int at = 0; at < brick_voxels; ++at) {
                    const auto k = static_cast<std::size_t>(at % (brick_side * brick_side));
                    const int up = layer * brick_side + (at / (brick_side * brick_side));
                    const double radius = layers.middle(up);
                    Voxel voxel = voxel_from(columns[k].sample(radius), band);
                    va[static_cast<std::size_t>(at)] = voxel;
                    const auto exact = base.temperature(voxel, surface[k], columns[k].ground(), radius);
                    const auto rough = base.temperature(voxel, surface[middle], coarse_ground, radius);
                    const auto pores = base.water(voxel, radius);
                    Voxel with_exact = voxel;
                    with_exact.temperature = static_cast<std::uint16_t>(voxel.temperature - exact);
                    with_exact.liquid_amount = static_cast<std::uint16_t>(voxel.liquid_amount - pores);
                    Voxel with_rough = voxel;
                    with_rough.temperature = static_cast<std::uint16_t>(voxel.temperature - rough);
                    with_rough.liquid_amount = with_exact.liquid_amount;
                    vb[static_cast<std::size_t>(at)] = with_exact;
                    vc[static_cast<std::size_t>(at)] = with_rough;
                    ++streams.voxels;
                    streams.off_exact += with_exact.temperature != 0 ? 1U : 0U;
                    streams.off_coarse += with_rough.temperature != 0 ? 1U : 0U;
                    streams.water_off += with_exact.liquid_amount != 0 ? 1U : 0U;
                }
                {
                    const auto le16 = [](std::vector<std::byte>& out, std::uint16_t v) {
                        out.push_back(static_cast<std::byte>(v & 0xFFU));
                        out.push_back(static_cast<std::byte>(v >> 8U));
                    };
                    for (int at = 0; at < brick_voxels; ++at) {
                        const auto u = static_cast<std::size_t>(at);
                        const auto dist = [&](int x, int y, int z) -> int {
                            return (x < 0 || y < 0 || z < 0)
                                       ? 0
                                       : int{vb[static_cast<std::size_t>((z * brick_side * brick_side) + (y * brick_side) + x)].distance};
                        };
                        const int x = at % brick_side;
                        const int y = (at / brick_side) % brick_side;
                        const int z = at / (brick_side * brick_side);
                        const int pred = dist(x - 1, y, z) + dist(x, y - 1, z) + dist(x, y, z - 1) -
                                         dist(x - 1, y - 1, z) - dist(x - 1, y, z - 1) -
                                         dist(x, y - 1, z - 1) + dist(x - 1, y - 1, z - 1);
                        le16(streams.p_dist, static_cast<std::uint16_t>(vb[u].distance));
                        le16(streams.p_pred, static_cast<std::uint16_t>(vb[u].distance - pred));
                        le16(streams.p_norm, vb[u].normal);
                        streams.p_rest.push_back(static_cast<std::byte>(vb[u].solid));
                        streams.p_rest.push_back(static_cast<std::byte>(vb[u].liquid));
                        le16(streams.p_rest, vb[u].liquid_amount);
                        le16(streams.p_rest, vb[u].temperature);
                    }
                }
                ++streams.bricks;
                Brick pb = Brick::pack(vb);
                Brick pc = Brick::pack(vc);
                run_a.add(a, Brick::pack(va), streams.same_a, streams.uniform_a);
                run_b.add(b, pb, streams.same_b, streams.uniform_b);
                run_c.add(c, pc, streams.same_c, streams.uniform_c);
                const auto rounded = [&](int step) {
                    auto vq = vb;
                    for (Voxel& voxel : vq) {
                        voxel.distance = static_cast<std::int16_t>(
                            static_cast<int>(std::lround(static_cast<double>(voxel.distance) / step)) * step);
                        voxel.normal = Voxel::no_normal;
                    }
                    return Brick::pack(vq);
                };
                for (const int step : {20, 50}) {
                    auto& store = step == 20 ? kept[static_cast<std::size_t>((bj * shard_bricks) + bi)].d20
                                             : kept[static_cast<std::size_t>((bj * shard_bricks) + bi)].d50;
                    for (const Voxel& voxel : vb) {
                        store.push_back(static_cast<std::int16_t>(
                            static_cast<int>(std::lround(static_cast<double>(voxel.distance) / step)) * step / step));
                    }
                }
                {
                    auto vn = vb;
                    for (Voxel& voxel : vn) {
                        const int clamped = std::clamp(int{voxel.distance}, -750, 750);
                        voxel.distance = static_cast<std::int16_t>(
                            static_cast<int>(std::lround(clamped / 20.0)) * 20);
                        voxel.normal = Voxel::no_normal;
                        kept[static_cast<std::size_t>((bj * shard_bricks) + bi)].n20.push_back(
                            static_cast<std::int16_t>(voxel.distance / 20));
                    }
                    run_q20n.add(q20n, Brick::pack(vn), same_q, uniform_q);
                }
                run_q10.add(q10, rounded(10), same_q, uniform_q);
                run_q20.add(q20, rounded(20), same_q, uniform_q);
                run_q50.add(q50, rounded(50), same_q, uniform_q);
                kept[static_cast<std::size_t>((bj * shard_bricks) + bi)].b.push_back(std::move(pb));
                kept[static_cast<std::size_t>((bj * shard_bricks) + bi)].c.push_back(std::move(pc));
            }
            run_a.flush(a);
            run_q10.flush(q10);
            run_q20.flush(q20);
            run_q50.flush(q50);
            run_q20n.flush(q20n);
            run_b.flush(b);
            run_c.flush(c);
        }
    }
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const auto at = [&](int xx, int yy) -> std::int32_t {
                return (xx < 0 || yy < 0) ? 0 : ground_mm[static_cast<std::size_t>((yy * side) + xx)];
            };
            const std::int32_t value = at(x, y);
            const std::int32_t residual = value - (at(x - 1, y) + at(x, y - 1) - at(x - 1, y - 1));
            for (const std::int32_t v : {residual}) {
                for (int byte = 0; byte < 4; ++byte) {
                    streams.p_height.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(v) >> (8 * byte)) & 0xFFU));
                }
            }
            for (int byte = 0; byte < 4; ++byte) {
                streams.p_height_raw.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(value) >> (8 * byte)) & 0xFFU));
            }
        }
    }
    // The ground at coarser precision and spacing: millimetres per step, columns between samples.
    constexpr std::array<std::array<int, 2>, 6> kinds{{{1, 1}, {10, 1}, {20, 1}, {10, 2}, {20, 2}, {50, 4}}};
    for (const auto& kind : kinds) {
        std::vector<std::byte> out;
        const int step = kind[0];
        const int spacing = kind[1];
        const int n = side / spacing;
        const auto q = [&](int x, int y) -> std::int32_t {
            if (x < 0 || y < 0) {
                return 0;
            }
            const std::int32_t mm = ground_mm[static_cast<std::size_t>((y * spacing * side) + (x * spacing))];
            return static_cast<std::int32_t>(std::lround(static_cast<double>(mm) / step));
        };
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const std::int32_t residual = q(x, y) - (q(x - 1, y) + q(x, y - 1) - q(x - 1, y - 1));
                for (int byte = 0; byte < 4; ++byte) {
                    out.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(residual) >> (8 * byte)) & 0xFFU));
                }
            }
        }
        streams.heights.push_back(std::move(out));
    }
    if (heights_only) {
        return streams;
    }
    streams.q10 = std::move(q10).take();
    streams.q20 = std::move(q20).take();
    streams.q50 = std::move(q50).take();
    streams.q20n = std::move(q20n).take();
    streams.a = std::move(a).take();
    streams.b = std::move(b).take();
    streams.c = std::move(c).take();
    // Grouped: every column's window padded up and down with its end bricks over the shard's
    // whole height, then any box of identical bricks written as one node.
    int lo = 1 << 30;
    int hi = -1;
    for (const Column& column : kept) {
        lo = std::min(lo, column.first);
        hi = std::max(hi, column.last);
    }
    const auto grouped = [&](bool coarse, std::vector<std::byte>& into, std::size_t& leaves,
                             std::size_t& solid) {
        Grid grid;
        grid.height = hi - lo + 1;
        grid.at.assign(static_cast<std::size_t>(shard_bricks * shard_bricks * grid.height), 0);
        for (int bj = 0; bj < shard_bricks; ++bj) {
            for (int bi = 0; bi < shard_bricks; ++bi) {
                Column& column = kept[static_cast<std::size_t>((bj * shard_bricks) + bi)];
                auto& source = coarse ? column.c : column.b;
                const std::size_t base = grid.bricks.size();
                for (Brick& brick : source) {
                    grid.bricks.push_back(brick);
                }
                for (int layer = lo; layer <= hi; ++layer) {
                    const int inside = std::clamp(layer, column.first, column.last) - column.first;
                    grid.ref(bi, bj, layer - lo) = base + static_cast<std::size_t>(inside);
                }
            }
        }
        hog::ByteWriter out;
        out.write(static_cast<std::uint16_t>(lo));
        out.write(static_cast<std::uint16_t>(grid.height));
        write_tree(out, grid, {0, shard_bricks, 0, shard_bricks, 0, grid.height}, leaves, solid);
        into = std::move(out).take();
    };
    // The shard as one block of rounded distances over the full height, each column held at its
    // end values above and below its own window; predicted from the neighbours in all three
    // directions, and only the differences kept.
    const auto block = [&](int mode, std::vector<std::byte>& into) {
        const int height = (hi - lo + 1) * brick_side;
        const int side_v = shard_bricks * brick_side;
        std::vector<std::int16_t> field(static_cast<std::size_t>(side_v * side_v * height), 0);
        const auto index = [&](int x, int y, int z) {
            return (static_cast<std::size_t>(z) * static_cast<std::size_t>(side_v * side_v)) +
                   (static_cast<std::size_t>(y) * static_cast<std::size_t>(side_v)) + static_cast<std::size_t>(x);
        };
        for (int bj = 0; bj < shard_bricks; ++bj) {
            for (int bi = 0; bi < shard_bricks; ++bi) {
                const Column& column = kept[static_cast<std::size_t>((bj * shard_bricks) + bi)];
                const auto& d = mode == 2 ? column.n20 : (mode == 1 ? column.d50 : column.d20);
                const int count = column.last - column.first + 1;
                for (int cy = 0; cy < brick_side; ++cy) {
                    for (int cx = 0; cx < brick_side; ++cx) {
                        for (int z = 0; z < height; ++z) {
                            const int layer = lo * brick_side + z;
                            const int inside = std::clamp(layer - (column.first * brick_side), 0, (count * brick_side) - 1);
                            const int brick = inside / brick_side;
                            const int within = inside % brick_side;
                            const std::size_t at = (static_cast<std::size_t>(brick) * brick_voxels) +
                                                   (static_cast<std::size_t>(within) * brick_side * brick_side) +
                                                   (static_cast<std::size_t>(cy) * brick_side) + static_cast<std::size_t>(cx);
                            field[index((bi * brick_side) + cx, (bj * brick_side) + cy, z)] = d[at];
                        }
                    }
                }
            }
        }
        const auto at = [&](int x, int y, int z) -> int {
            return (x < 0 || y < 0 || z < 0) ? 0 : int{field[index(x, y, z)]};
        };
        std::vector<std::byte> out;
        for (int z = 0; z < height; ++z) {
            for (int y = 0; y < side_v; ++y) {
                for (int x = 0; x < side_v; ++x) {
                    const int pred = at(x - 1, y, z) + at(x, y - 1, z) + at(x, y, z - 1) - at(x - 1, y - 1, z) -
                                     at(x - 1, y, z - 1) - at(x, y - 1, z - 1) + at(x - 1, y - 1, z - 1);
                    const auto residual = static_cast<std::uint16_t>(at(x, y, z) - pred);
                    out.push_back(static_cast<std::byte>(residual & 0xFFU));
                    out.push_back(static_cast<std::byte>(residual >> 8U));
                }
            }
        }
        into = std::move(out);
    };
    block(0, streams.r20);
    block(1, streams.r50);
    block(2, streams.rn20);
    grouped(false, streams.d, streams.leaves_d, streams.uniform_leaves_d);
    grouped(true, streams.e, streams.leaves_e, streams.uniform_leaves_e);
    return streams;
}

std::string env(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : fallback;
}

// Writes a variant's bytes, or stops the run: a full disk would otherwise cut the files short and
// the sizes would quietly read low. Only the variants in HOG_BENCH_ONLY (comma separated), if set.
void put(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    static const std::string only = env("HOG_BENCH_ONLY", "");
    const std::string variant = path.parent_path().filename().string();
    if (!only.empty() && ("," + only + ",").find("," + variant + ",") == std::string::npos) {
        return;
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
        std::fprintf(stderr, "writing %s failed: is the disk full?\n", path.c_str());
        std::abort();
    }
}

} // namespace

TEST_CASE("how big the stored world is, on a random sample of shards", "[.store-size]") {
    const std::filesystem::path dir = env("HOG_BENCH_DIR", "");
    REQUIRE_FALSE(dir.empty());
    const auto sample = static_cast<std::uint64_t>(std::stoull(env("HOG_BENCH_SAMPLE", "512")));
    const auto threads = static_cast<unsigned>(std::stoul(env("HOG_BENCH_THREADS", "8")));
    Planet planet = preset("earth").value_or(Planet{});
    planet.radius = std::stod(env("HOG_BENCH_RADIUS", "15000"));
    const Generator generator{planet};
    const Baseline base{.planet = &planet,
                        .geotherm = generator.derived().geotherm,
                        .lapse = planet.pressure > 0.0 ? earth_lapse * planet.gravity / 9.81 : 0.0};
    const int level = generator.layers().level;
    const int shards_across = ((1 << level) / brick_side) / shard_bricks;
    REQUIRE(shards_across > 0);
    const auto across = static_cast<std::uint64_t>(shards_across);
    const std::uint64_t total = std::uint64_t{6} * across * across;
    std::printf("level %d, voxel %.3f m thick, %d shards a side, %llu shards, sampling 1 in %llu\n",
                level, generator.layers().thickness, shards_across,
                static_cast<unsigned long long>(total), static_cast<unsigned long long>(sample));
    const bool only_heights = env("HOG_BENCH_HEIGHTS", "") == "1";
    for (const char* name : {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "h0", "h1", "h2", "h3", "h4", "h5", "q10", "q20", "q50", "r20", "r50", "rn20", "q20n"}) {
        std::filesystem::create_directories(dir / name);
    }
    struct Job { int face, si, sj; };
    std::vector<Job> jobs;
    for (int face = 0; face < 6; ++face) {
        for (int si = 0; si < shards_across; ++si) {
            for (int sj = 0; sj < shards_across; ++sj) {
                if (hog::hash_values(planet.seed, 0x5348ULL, face, si, sj) % sample == 0) {
                    jobs.push_back({face, si, sj});
                }
            }
        }
    }
    std::printf("%zu shards in the sample\n", jobs.size());
    std::atomic<std::size_t> next{0};
    std::mutex mutex;
    Streams sum;
    std::size_t done = 0;
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (std::size_t k = next++; k < jobs.size(); k = next++) {
                const Job job = jobs[k];
                const Streams made = build_shard(generator, base, job.face, job.si, job.sj, only_heights);
                const std::string name = std::to_string(job.face) + "_" + std::to_string(job.si) +
                                         "_" + std::to_string(job.sj) + ".shard";
                if (only_heights) {
                    for (std::size_t h = 0; h < made.heights.size(); ++h) {
                        put(dir / ("h" + std::to_string(h)) / name, made.heights[h]);
                    }
                    const std::lock_guard lock{mutex};
                    ++done;
                    continue;
                }
                put(dir / "a" / name, made.a);
                put(dir / "q10" / name, made.q10);
                put(dir / "r20" / name, made.r20);
                put(dir / "rn20" / name, made.rn20);
                put(dir / "q20n" / name, made.q20n);
                put(dir / "r50" / name, made.r50);
                put(dir / "q20" / name, made.q20);
                put(dir / "q50" / name, made.q50);
                put(dir / "b" / name, made.b);
                put(dir / "c" / name, made.c);
                put(dir / "d" / name, made.d);
                put(dir / "e" / name, made.e);
                put(dir / "f" / name, made.p_dist);
                put(dir / "g" / name, made.p_pred);
                put(dir / "h" / name, made.p_norm);
                put(dir / "i" / name, made.p_rest);
                put(dir / "j" / name, made.p_height);
                put(dir / "k" / name, made.p_height_raw);
                const std::lock_guard lock{mutex};
                sum.bricks += made.bricks;
                sum.same_a += made.same_a; sum.same_b += made.same_b; sum.same_c += made.same_c;
                sum.uniform_a += made.uniform_a; sum.uniform_b += made.uniform_b;
                sum.uniform_c += made.uniform_c;
                sum.voxels += made.voxels; sum.off_exact += made.off_exact;
                sum.off_coarse += made.off_coarse; sum.water_off += made.water_off;
                sum.leaves_d += made.leaves_d; sum.leaves_e += made.leaves_e;
                sum.uniform_leaves_d += made.uniform_leaves_d; sum.uniform_leaves_e += made.uniform_leaves_e;
                if (++done % 100 == 0) {
                    std::printf("  %zu / %zu shards\n", done, jobs.size());
                    std::fflush(stdout);
                }
            }
        });
    }
    for (std::thread& worker : pool) {
        worker.join();
    }
    std::printf("bricks %zu; bricks folded into the one before: a %zu b %zu c %zu; uniform stored: a %zu b %zu c %zu\n",
                sum.bricks, sum.same_a, sum.same_b, sum.same_c, sum.uniform_a, sum.uniform_b, sum.uniform_c);
    std::printf("voxels %zu; temperature off baseline: exact %.4f%% coarse %.4f%%; pore water off baseline %.4f%%\n",
                sum.voxels, 100.0 * static_cast<double>(sum.off_exact) / static_cast<double>(sum.voxels),
                100.0 * static_cast<double>(sum.off_coarse) / static_cast<double>(sum.voxels),
                100.0 * static_cast<double>(sum.water_off) / static_cast<double>(sum.voxels));
    std::printf("grouped nodes: exact %zu (%zu uniform), coarse %zu (%zu uniform), against %zu bricks\n", sum.leaves_d, sum.uniform_leaves_d, sum.leaves_e, sum.uniform_leaves_e, sum.bricks);
    std::printf("sampled %zu of %llu shards\n", jobs.size(), static_cast<unsigned long long>(total));
    CHECK(done == jobs.size());
}
