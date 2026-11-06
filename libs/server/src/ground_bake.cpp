#include <hog/server/ground_bake.hpp>

#include <hog/core/bytes.hpp>
#include <hog/flow/simulation.hpp>
#include <hog/globe/cell.hpp>

#include <hog/core/hash.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <queue>
#include <format>
#include <utility>

namespace hog::server {
namespace {

constexpr double hours_a_year = 365.25 * 24.0;

// Tiles across a face.
std::uint32_t tiles_across(const globe::VoxelStore& store) {
    return std::max<std::uint32_t>(1, store.bricks_across() / GroundBake::tile_side);
}

} // namespace

GroundBake::Tile GroundBake::tile_at(const globe::VoxelStore& store, DVec3 up) {
    const globe::Layers& layers = store.layers();
    const globe::Cell cell = globe::cell_under(up, layers.level);
    const std::uint32_t across = globe::brick_side * tile_side;
    const std::uint32_t last = tiles_across(store) - 1;
    return {.face = cell.face,
            .i = std::min(cell.i / across, last),
            .j = std::min(cell.j / across, last)};
}

std::vector<GroundBake::Tile> GroundBake::due(const globe::VoxelStore& store,
                                              std::span<const DVec3> places, int reach) const {
    const auto across = static_cast<std::int64_t>(tiles_across(store));
    std::vector<Tile> found;
    std::set<Tile> seen;
    const auto consider = [&](const Tile& home, int di, int dj) {
        const std::int64_t i = std::int64_t{home.i} + di;
        const std::int64_t j = std::int64_t{home.j} + dj;
        // Tiles past the face's edge wait for a place on the face beside.
        if (i < 0 || j < 0 || i >= across || j >= across) {
            return;
        }
        const Tile tile{
            .face = home.face, .i = static_cast<std::uint32_t>(i), .j = static_cast<std::uint32_t>(j)};
        if (!baked(tile) && seen.insert(tile).second) {
            found.push_back(tile);
        }
    };
    for (int ring = 0; ring <= reach; ++ring) {
        for (const DVec3 up : places) {
            const Tile home = tile_at(store, up);
            // The ring's edge: its top and bottom rows whole, its sides between.
            for (int d = -ring; d <= ring; ++d) {
                consider(home, d, -ring);
                if (ring > 0) {
                    consider(home, d, ring);
                }
            }
            for (int d = 1 - ring; d < ring; ++d) {
                consider(home, -ring, d);
                consider(home, ring, d);
            }
        }
    }
    return found;
}

std::vector<globe::BrickSite> GroundBake::bricks_of(const globe::World& world, Tile tile) {
    const globe::Generator& generator = world.generator();
    const globe::Layers& layers = generator.layers();
    const globe::VoxelStore& store = world.store();
    std::vector<globe::BrickSite> bricks;
    for (std::uint32_t a = 0; a < tile_side; ++a) {
        for (std::uint32_t b = 0; b < tile_side; ++b) {
            const std::uint32_t bi = (tile.i * tile_side) + a;
            const std::uint32_t bj = (tile.j * tile_side) + b;
            if (bi >= store.bricks_across() || bj >= store.bricks_across()) {
                continue;
            }
            // The stack's middle column tells where its ground is, give or take its slopes.
            const globe::Cell cell{.level = static_cast<std::uint8_t>(layers.level),
                                   .face = tile.face,
                                   .i = (bi * globe::brick_side) + (globe::brick_side / 2),
                                   .j = (bj * globe::brick_side) + (globe::brick_side / 2)};
            const std::vector<globe::Generator::Column> columns =
                generator.columns(std::span{&cell, 1}, globe::voxel_lod(generator));
            const int highest = globe::highest_layer(world, cell, columns[0]);
            const double ground =
                globe::surface_under(world, globe::center(cell), layers.top(highest))
                    .value_or(layers.floor);
            const int low = std::max(0, layers.layer_at(ground) - (2 * globe::brick_side));
            const int high = std::min(layers.count - 1, highest + globe::brick_side);
            for (int layer = low / globe::brick_side; layer <= high / globe::brick_side; ++layer) {
                bricks.push_back({.face = tile.face,
                                  .i = bi,
                                  .j = bj,
                                  .layer = static_cast<std::uint32_t>(layer)});
            }
        }
    }
    return bricks;
}

std::vector<GroundBake::Baked> GroundBake::held_in(const globe::VoxelStore& store, Tile tile) {
    std::vector<Baked> held;
    for (std::uint32_t a = 0; a < tile_side; ++a) {
        for (std::uint32_t b = 0; b < tile_side; ++b) {
            const globe::BrickSite stack{
                .face = tile.face, .i = (tile.i * tile_side) + a, .j = (tile.j * tile_side) + b};
            if (stack.i >= store.bricks_across() || stack.j >= store.bricks_across()) {
                continue;
            }
            for (const std::uint32_t layer : store.layers_held(stack)) {
                globe::BrickSite site = stack;
                site.layer = layer;
                held.push_back({.site = site, .brick = *store.brick(site)});
            }
        }
    }
    return held;
}

std::vector<GroundBake::Baked> GroundBake::bake(const globe::Generator& generator,
                                                const std::vector<Baked>& held, Tile tile,
                                                const RainRecord& rain, double evaporation) {
    globe::VoxelStore store{generator.layers()};
    for (const Baked& one : held) {
        if (one.brick) {
            store.put(one.site, *one.brick);
        }
    }
    store.clear_changed();
    globe::World world{generator, store};
    flow::Simulation flow{world};
    const Course through = course(world, tile, rain);
    const auto river = through.find(tile);
    static_cast<void>(flow.settle_water(
        bricks_of(world, tile),
        {.rain = [&](DVec3 up) { return rain.at(up); },
         .river = river != through.end() ? std::optional{river->second} : std::nullopt,
         .springs = springs(tile),
         .evaporation = evaporation}));
    std::vector<Baked> written;
    for (const globe::BrickSite site : store.changed()) {
        const globe::Brick* brick = store.brick(site);
        written.push_back({.site = site,
                           .brick = brick != nullptr ? std::optional{*brick} : std::nullopt});
    }
    return written;
}

namespace {

// Metres of rain a year that run off rather than soak in, of each metre.
constexpr double runs_off = 0.7;
// A river is a drainage's that gathers this many square metres of ground or more, where streams
// run the year round.
constexpr double river_catchment = 5.0e5;

// The side of a tile `di`, `dj` points out of: +i, +j, -i, -j.
std::size_t side_towards(std::int64_t di, std::int64_t dj) {
    if (std::abs(di) >= std::abs(dj)) {
        return di >= 0 ? 0 : 2;
    }
    return dj >= 0 ? 1 : 3;
}

DVec3 middle_of(const globe::Layers& layers, GroundBake::Tile tile) {
    const std::uint32_t across = globe::brick_side * GroundBake::tile_side;
    return globe::center({.level = static_cast<std::uint8_t>(layers.level),
                          .face = tile.face,
                          .i = (tile.i * across) + (across / 2),
                          .j = (tile.j * across) + (across / 2)});
}

// The tiles a drainage cell covers, each with its ground from its voxels.
class TileGround {
public:
    TileGround(const globe::World& world, globe::Cell cell) : face_{cell.face} {
        const globe::Layers& layers = world.store().layers();
        const int finer = layers.level - cell.level;
        const std::int64_t per_tile = std::int64_t{globe::brick_side} * GroundBake::tile_side;
        first_i_ = (std::int64_t{cell.i} << finer) / per_tile;
        first_j_ = (std::int64_t{cell.j} << finer) / per_tile;
        wide_ = ((((std::int64_t{cell.i} + 1) << finer) - 1) / per_tile) - first_i_ + 1;
        along_ = ((((std::int64_t{cell.j} + 1) << finer) - 1) / per_tile) - first_j_ + 1;
        ground_.resize(static_cast<std::size_t>(wide_ * along_));
        for (std::size_t n = 0; n < ground_.size(); ++n) {
            ground_[n] = globe::top_of(world, middle_of(layers, tile(n))).value_or(layers.floor);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return ground_.size(); }
    [[nodiscard]] double ground(std::size_t n) const { return ground_[n]; }
    [[nodiscard]] GroundBake::Tile tile(std::size_t n) const {
        return {.face = face_,
                .i = static_cast<std::uint32_t>(first_i_ + a(n)),
                .j = static_cast<std::uint32_t>(first_j_ + b(n))};
    }
    // The tile `da`, `db` across from one, if it's in the cell.
    [[nodiscard]] std::optional<std::size_t> beside(std::size_t n, int da, int db) const {
        const std::int64_t to_a = a(n) + da;
        const std::int64_t to_b = b(n) + db;
        if (to_a < 0 || to_b < 0 || to_a >= wide_ || to_b >= along_) {
            return std::nullopt;
        }
        return static_cast<std::size_t>((to_b * wide_) + to_a);
    }
    // The lowest tile along the cell's edge on `side`.
    [[nodiscard]] std::size_t lowest_on(std::size_t side) const {
        std::size_t best = size();
        for (std::size_t n = 0; n < size(); ++n) {
            if (on_edge(n, side) && (best == size() || ground_[n] < ground_[best])) {
                best = n;
            }
        }
        return best;
    }
    // The side of `n` its neighbour `from` lies on.
    [[nodiscard]] std::size_t side_of(std::size_t n, std::size_t from) const {
        return side_towards(a(from) - a(n), b(from) - b(n));
    }

private:
    [[nodiscard]] std::int64_t a(std::size_t n) const { return static_cast<std::int64_t>(n) % wide_; }
    [[nodiscard]] std::int64_t b(std::size_t n) const { return static_cast<std::int64_t>(n) / wide_; }
    [[nodiscard]] bool on_edge(std::size_t n, std::size_t side) const {
        switch (side) {
        case 0: return a(n) == wide_ - 1;
        case 1: return b(n) == along_ - 1;
        case 2: return a(n) == 0;
        default: return b(n) == 0;
        }
    }

    std::uint8_t face_ = 0;
    std::int64_t first_i_ = 0;
    std::int64_t first_j_ = 0;
    std::int64_t wide_ = 1;
    std::int64_t along_ = 1;
    std::vector<double> ground_;
};

// The lowest way from `start` to `end`: each step costs the ground it climbs onto, and a little,
// so a flat way is short. Each tile's step back toward `start`, `size()` at the start.
std::vector<std::size_t> lowest_way(const TileGround& tiles, std::size_t start, std::size_t end,
                                    double step) {
    std::vector<double> cost(tiles.size(), std::numeric_limits<double>::max());
    std::vector<std::size_t> came(tiles.size(), tiles.size());
    using Waiting = std::pair<double, std::size_t>;
    std::priority_queue<Waiting, std::vector<Waiting>, std::greater<>> waiting;
    cost[start] = 0.0;
    waiting.emplace(0.0, start);
    constexpr std::array<std::pair<int, int>, 4> steps{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
    while (!waiting.empty()) {
        const auto [spent, here] = waiting.top();
        waiting.pop();
        if (here == end) {
            break;
        }
        if (spent > cost[here]) {
            continue;
        }
        for (const auto& [da, db] : steps) {
            const std::optional<std::size_t> next = tiles.beside(here, da, db);
            if (!next) {
                continue;
            }
            const double through =
                spent + std::max(0.0, tiles.ground(*next) - tiles.ground(here)) + step;
            if (through < cost[*next]) {
                cost[*next] = through;
                came[*next] = here;
                waiting.emplace(through, *next);
            }
        }
    }
    return came;
}

// The sides of a drainage cell its river comes in and goes out by: towards the neighbour on its
// face giving it most, and towards the one it drains into; none for a cell that isn't a river's.
std::optional<std::pair<std::size_t, std::size_t>> river_ends(const globe::Generator::Shape& shape,
                                                              globe::Cell home) {
    const auto index = static_cast<std::uint32_t>(globe::index_of(home));
    if (static_cast<double>(shape.catchment[index]) < river_catchment ||
        shape.receiver[index] == index) {
        return std::nullopt;
    }
    std::optional<globe::Cell> upstream;
    double most = 0.0;
    const globe::CellNeighbors beside = globe::around(home);
    for (const globe::Cell cell : beside.all()) {
        const auto at = static_cast<std::uint32_t>(globe::index_of(cell));
        const auto gives = static_cast<double>(shape.catchment[at]);
        if (cell.face == home.face && shape.receiver[at] == index && gives > most) {
            most = gives;
            upstream = cell;
        }
    }
    // Rivers across a face's edge wait for the cells to be on one face.
    const globe::Cell downstream = globe::cell_at_index(shape.cut_level, shape.receiver[index]);
    if (!upstream || downstream.face != home.face) {
        return std::nullopt;
    }
    return std::pair{
        side_towards(std::int64_t{upstream->i} - home.i, std::int64_t{upstream->j} - home.j),
        side_towards(std::int64_t{downstream.i} - home.i, std::int64_t{downstream.j} - home.j)};
}

} // namespace

GroundBake::Course GroundBake::course(const globe::World& world, Tile tile,
                                      const RainRecord& rain) {
    const globe::Generator& generator = world.generator();
    const globe::Layers& layers = generator.layers();
    const globe::Generator::Shape& shape = generator.shape();
    if (shape.receiver.empty()) {
        return {};
    }
    const DVec3 middle = middle_of(layers, tile);
    const globe::Cell home = globe::cell_under(middle, shape.cut_level);
    const auto ends = river_ends(shape, home);
    if (!ends) {
        return {};
    }
    const auto [in_side, out_side] = *ends;
    const TileGround tiles{world, home};
    const std::size_t start = tiles.lowest_on(in_side);
    const std::size_t end = tiles.lowest_on(out_side);
    const std::vector<std::size_t> came = lowest_way(tiles, start, end, layers.thickness);
    const double discharge =
        static_cast<double>(shape.catchment[globe::index_of(home)]) * rain.at(middle) * runs_off;
    Course found;
    for (std::size_t here = end; here != tiles.size(); here = came[here]) {
        const std::size_t side = here == start ? in_side : tiles.side_of(here, came[here]);
        found.emplace(tiles.tile(here), flow::Region::River{.discharge = discharge, .side = side});
    }
    return found;
}

std::vector<flow::Region::Spring> GroundBake::springs(Tile tile) {
    // About one tile in eight has a spring, giving from a trickle to a few litres a second.
    const std::uint64_t seed = hash_values(std::uint64_t{tile.face}, std::uint64_t{tile.i},
                                           std::uint64_t{tile.j}, std::uint64_t{0x5eed});
    const auto unit = [&](std::uint64_t salt) {
        return static_cast<double>(hash_values(seed, salt) >> 11U) * 0x1.0p-53;
    };
    if (unit(1) > 0.125) {
        return {};
    }
    const std::uint32_t across = globe::brick_side * tile_side;
    const auto pick = [&](std::uint64_t salt) {
        return static_cast<std::uint32_t>(unit(salt) * static_cast<double>(across));
    };
    return {{.face = tile.face,
             .i = (tile.i * across) + pick(2),
             .j = (tile.j * across) + pick(3),
             .discharge = std::pow(10.0, 3.0 + (2.0 * unit(4)))}};
}

std::vector<std::byte> GroundBake::saved() const {
    ByteWriter out;
    out.write_varint(baked_.size());
    for (const Tile& tile : baked_) {
        out.write(tile.face);
        out.write_varint(tile.i);
        out.write_varint(tile.j);
    }
    return std::move(out).take();
}

Result<void> GroundBake::restore(std::span<const std::byte> bytes) {
    ByteReader in{bytes};
    const auto count = in.read_varint();
    if (!count) {
        return std::unexpected(count.error());
    }
    std::set<Tile> read;
    for (std::uint64_t k = 0; k < *count; ++k) {
        const auto face = in.read<std::uint8_t>();
        const auto i = in.read_varint();
        const auto j = in.read_varint();
        if (!face || !i || !j) {
            return make_error(ErrorCode::Parse, "baked tiles cut short");
        }
        if (*face >= 6) {
            return make_error(ErrorCode::Parse, std::format("no face {}", *face));
        }
        read.insert({.face = *face,
                     .i = static_cast<std::uint32_t>(*i),
                     .j = static_cast<std::uint32_t>(*j)});
    }
    baked_ = std::move(read);
    return {};
}

RainRecord::RainRecord(int level, double otherwise)
    : level_{level}, otherwise_{otherwise},
      sum_(static_cast<std::size_t>(globe::cells_at(level)), 0.0),
      hours_(static_cast<std::size_t>(globe::cells_at(level)), 0.0) {}

void RainRecord::add_cell(std::size_t cell, double mm_an_hour, double hours) {
    sum_[cell] += mm_an_hour * hours;
    hours_[cell] += hours;
}

DVec3 RainRecord::middle_of(std::size_t cell) const {
    return globe::center(globe::cell_at_index(level_, cell));
}

double RainRecord::at(DVec3 up) const {
    const auto cell = static_cast<std::size_t>(globe::index_of(globe::cell_under(up, level_)));
    if (hours_[cell] <= 0.0) {
        return otherwise_;
    }
    return sum_[cell] / hours_[cell] * hours_a_year / 1'000.0;
}

} // namespace hog::server
