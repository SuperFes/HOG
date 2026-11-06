#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <cstdio>
#include <map>
#include <vector>

namespace {

using namespace hog::globe;

// A column's voxels as runs of one kind: from layer `start`, `kind` until the next run.
struct Run {
    int start;
    std::uint32_t kind;
};

using Runs = std::vector<Run>;

// The same cubes shared: a node that matches another, children and all, is one node.
class Dag {
public:
    Dag(const std::vector<Runs>& columns, int across, int layers)
        : columns_{&columns}, across_{across}, layers_{layers} {}

    struct Total {
        std::int64_t tree = 0;   // nodes with no sharing, leaves included
        std::int64_t shared = 0; // distinct nodes
    };

    Total count(int side) {
        for (int z = 0; z < layers_; z += side) {
            build(0, 0, z, side);
        }
        return {.tree = tree_, .shared = static_cast<std::int64_t>(ids_.size())};
    }

private:
    static constexpr std::int64_t leaf_base = -1000;

    int uniform(int x0, int y0, int z0, int side) const {
        int kind = -1;
        for (int y = y0; y < std::min(y0 + side, across_); ++y) {
            for (int x = x0; x < std::min(x0 + side, across_); ++x) {
                const Runs& runs = (*columns_)[static_cast<std::size_t>((y * across_) + x)];
                const auto next = std::ranges::upper_bound(runs, z0, {}, &Run::start);
                const Run& run = *(next - 1);
                const int end = next == runs.end() ? layers_ : next->start;
                if (end < std::min(z0 + side, layers_)) {
                    return -1;
                }
                if (kind == -1) {
                    kind = static_cast<int>(run.kind);
                } else if (kind != static_cast<int>(run.kind)) {
                    return -1;
                }
            }
        }
        return kind;
    }

    // The node's id: the same for equal nodes. A leaf is its kind; a node is its children's ids.
    std::int64_t build(int x0, int y0, int z0, int side) {
        ++tree_;
        if (x0 >= across_ || y0 >= across_ || z0 >= layers_) {
            return -1;
        }
        const int kind = uniform(x0, y0, z0, side);
        if (kind >= 0) {
            return intern({leaf_base, kind, 0, 0, 0, 0, 0, 0, side});
        }
        const int half = side / 2;
        std::array<std::int64_t, 9> key{};
        std::size_t k = 0;
        for (int dz = 0; dz < 2; ++dz) {
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    key.at(k++) = build(x0 + (dx * half), y0 + (dy * half), z0 + (dz * half), half);
                }
            }
        }
        key.at(8) = side;
        return intern(key);
    }

    std::int64_t intern(const std::array<std::int64_t, 9>& key) {
        const auto [found, added] = ids_.try_emplace(key, static_cast<std::int64_t>(ids_.size()));
        return found->second;
    }

    const std::vector<Runs>* columns_;
    int across_;
    int layers_;
    std::int64_t tree_ = 0;
    std::map<std::array<std::int64_t, 9>, std::int64_t> ids_;
};

// A voxel as it would be stored: its materials and how full of each it is, in eighths. What the
// generator can say from the voxel's place alone (distance, normal, temperature) is left out.
std::uint32_t stored_key(const Voxel& voxel, std::map<std::uint64_t, std::uint32_t>& ids) {
    const auto eighth = [](std::uint16_t amount) { return static_cast<std::uint64_t>(amount >> 12U); };
    const std::uint64_t key = static_cast<std::uint64_t>(voxel.solid) |
                              (eighth(voxel.solid_amount) << 8U) |
                              (static_cast<std::uint64_t>(voxel.grain) << 16U) |
                              (eighth(voxel.grain_amount) << 24U) |
                              (static_cast<std::uint64_t>(voxel.liquid) << 32U) |
                              (eighth(voxel.liquid_amount) << 40U);
    return ids.try_emplace(key, static_cast<std::uint32_t>(ids.size())).first->second;
}

void report(const char* label, const char* what, const std::vector<Runs>& columns, int across,
            int layers) {
    const double count = static_cast<double>(across) * across;
    Dag dag{columns, across, layers};
    const Dag::Total total = dag.count(128);
    std::int64_t changes = 0;
    for (const Runs& runs : columns) {
        changes += static_cast<std::int64_t>(runs.size()) - 1;
    }
    std::printf("%-9s %-8s %5.1f changes/column, nodes/column: %6.2f as a tree, %.3f shared (%.0fx)\n",
                label, what, static_cast<double>(changes) / count,
                static_cast<double>(total.tree) / count, static_cast<double>(total.shared) / count,
                static_cast<double>(total.tree) / static_cast<double>(total.shared));
}

// The patch's columns over all their depth, as runs of material alone, and as runs of the voxel
// as it would be stored.
void measure(const Generator& generator, const char* label, hog::DVec3 at, int across) {
    const Layers& layers = generator.layers();
    VoxelStore store{layers};
    const World world{generator, store};
    const Cell centre = cell_under(at, layers.level);
    std::vector<Runs> material;
    std::vector<Runs> voxels;
    std::map<std::uint64_t, std::uint32_t> ids;
    for (int b = 0; b < across; ++b) {
        for (int a = 0; a < across; ++a) {
            const Cell cell{.level = centre.level,
                            .face = centre.face,
                            .i = centre.i - static_cast<std::uint32_t>(across / 2) +
                                 static_cast<std::uint32_t>(a),
                            .j = centre.j - static_cast<std::uint32_t>(across / 2) +
                                 static_cast<std::uint32_t>(b)};
            const std::vector<Generator::Column> columns{
                generator.column(center(cell), voxel_lod(generator))};
            const std::vector<Voxel> filled = world.fill(std::array{cell}, columns, 0, layers.count, 1);
            Runs by_material;
            Runs by_voxel;
            for (int layer = 0; layer < layers.count; ++layer) {
                const Voxel& voxel = filled[static_cast<std::size_t>(layer)];
                const std::uint32_t kind =
                    voxel.solid_amount >= Voxel::full / 2 ? static_cast<std::uint32_t>(voxel.solid)
                    : voxel.liquid_amount >= Voxel::full / 2
                        ? static_cast<std::uint32_t>(Material::Water)
                        : static_cast<std::uint32_t>(Material::Air);
                if (by_material.empty() || by_material.back().kind != kind) {
                    by_material.push_back({layer, kind});
                }
                const std::uint32_t stored = stored_key(voxel, ids);
                if (by_voxel.empty() || by_voxel.back().kind != stored) {
                    by_voxel.push_back({layer, stored});
                }
            }
            material.push_back(std::move(by_material));
            voxels.push_back(std::move(by_voxel));
        }
    }
    report(label, "material", material, across, layers.count);
    report(label, "voxel", voxels, across, layers.count);
}

// How many leaves a patch needs when every uniform cube is one. Hidden: it samples whole columns.
TEST_CASE("octree leaves per column over land, coast and sea", "[.octree_estimate]") {
    const Generator generator{Planet{}};
    const double sea = generator.sea_level().value_or(generator.planet().radius);
    hog::DVec3 peak{0.0, 0.0, 1.0};
    hog::DVec3 coast{0.0, 0.0, 1.0};
    hog::DVec3 deep{0.0, 0.0, 1.0};
    double highest = 0.0;
    double nearest = 1e18;
    double lowest = 1e18;
    for (int k = 0; k < 40000; ++k) {
        const double z = 1.0 - ((2.0 * k + 1.0) / 40000.0);
        const double r = std::sqrt(1.0 - (z * z));
        const hog::DVec3 d{r * std::cos(2.399963 * k), r * std::sin(2.399963 * k), z};
        const double ground = generator.column(d, 200.0).ground();
        if (ground > highest) {
            highest = ground;
            peak = d;
        }
        if (std::abs(ground - sea) < nearest) {
            nearest = std::abs(ground - sea);
            coast = d;
        }
        if (ground < lowest) {
            lowest = ground;
            deep = d;
        }
    }
    measure(generator, "mountain", peak, 128);
    measure(generator, "coast", coast, 128);
    measure(generator, "deep sea", deep, 128);
}

} // namespace
