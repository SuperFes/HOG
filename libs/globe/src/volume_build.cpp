#include <hog/core/assert.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hog::globe {
namespace {

// Layers `low` to `high` of a column, all one voxel.
struct Run {
    int low = 0;
    int high = 0;
    Cube voxel;
};

// What the generator makes is as warm as the place, so it holds no heat over it.
Cube made(const Generator::Column& column, const Layers& layers, int layer, bool caves) {
    const Voxel voxel = voxel_from(column.sample(layers.middle(layer), caves), layers.thickness);
    return cube_of(voxel, voxel.temperature / 8.0);
}

// Voxels wider than a tunnel show no cave: caves are seen from inside them, where the voxels are
// finest.
constexpr double widest_cave = 7.0;

// A voxel deep in the ground, clear of its surface, caves and fallen rock: the bed it shows, and
// the water in its pores under open water, as made() has it.
Cube deep(const Generator::Column& column, const Planet& planet, const Layers& layers, int layer) {
    const double radius = layers.middle(layer);
    Sample sample;
    sample.distance = radius - column.ground();
    sample.material = column.inside(radius);
    if (column.water() > 0.0 && radius < column.water()) {
        sample.liquid = planet.volatiles == Volatile::Water ? Material::Water : Material::Air;
        sample.water = properties(sample.material).porosity;
    }
    const Voxel voxel = voxel_from(sample, layers.thickness);
    return cube_of(voxel, voxel.temperature / 8.0);
}

// The column's voxels from `bottom` to `top` as runs: one sample for each stretch between the
// generator's boundaries, and one for each voxel within them; in the ground between them, each
// voxel's bed.
std::vector<Run> runs_of(const Generator::Column& column, const Planet& planet,
                         const Layers& layers, int bottom, int top, bool caves) {
    // Layers a boundary passes through, with whether a cave may be in them.
    struct Changing {
        int low = 0;
        int high = 0;
        bool cave = false;
    };
    std::vector<Changing> changing;
    for (const Generator::Column::Boundary& change :
         column.boundaries(layers.bottom(bottom), layers.top(top), 0.0, false, caves)) {
        const int low = std::max(layers.layer_at(change.bottom), bottom);
        const int high = std::min(layers.layer_at(change.top), top);
        if (low > high) {
            continue;
        }
        if (!changing.empty() && low <= changing.back().high + 1 &&
            change.cave == changing.back().cave) {
            changing.back().high = std::max(changing.back().high, high);
        } else if (!changing.empty() && low <= changing.back().high) {
            // Overlapping stretches are sampled for caves if either may hold one.
            changing.back().high = std::max(changing.back().high, high);
            changing.back().cave = true;
        } else {
            changing.push_back({.low = low, .high = high, .cave = change.cave});
        }
    }
    std::vector<Run> runs;
    const auto add = [&](int low, int high, const Cube& voxel) {
        if (!runs.empty() && runs.back().voxel == voxel) {
            runs.back().high = high;
        } else {
            runs.push_back({.low = low, .high = high, .voxel = voxel});
        }
    };
    // Between boundaries all is alike but the beds, which lie only in the ground.
    const auto between = [&](int low, int high) {
        if (layers.middle(high) >= column.ground()) {
            add(low, high, made(column, layers, (low + high) / 2, false));
            return;
        }
        for (int layer = low; layer <= high; ++layer) {
            add(layer, layer, deep(column, planet, layers, layer));
        }
    };
    int next = bottom;
    for (const Changing& stretch : changing) {
        if (next < stretch.low) {
            between(next, stretch.low - 1);
        }
        for (int layer = std::max(stretch.low, next); layer <= stretch.high; ++layer) {
            add(layer, layer, made(column, layers, layer, stretch.cave));
        }
        next = std::max(next, stretch.high + 1);
    }
    if (next <= top) {
        between(next, top);
    }
    return runs;
}

// One tile's voxels, from its columns' runs between `bottom` and `top`: nothing under `bottom`,
// and from `keep` up what the tile already held.
struct TileSource {
    static constexpr int side = Volume::tile_side;

    const std::vector<std::vector<Run>>* runs = nullptr;
    const VoxelTree* kept = nullptr;
    int base = 0; // the tile's lowest layer
    int bottom = 0;
    int top = 0;
    int keep = 0;

    // The voxel of the column at (x, y) in `layer`, and the highest layer it runs on to.
    [[nodiscard]] const Run& run_at(int x, int y, int layer) const {
        const std::vector<Run>& up =
            (*runs)[(static_cast<std::size_t>(y) * side) + static_cast<std::size_t>(x)];
        return *std::ranges::lower_bound(up, layer, {}, &Run::high);
    }

    [[nodiscard]] std::optional<Cube> kept_whole(int x, int y, int z, int across) const {
        if (kept == nullptr) {
            return Cube{};
        }
        return kept->leaf_side(x, y, z) >= across ? std::optional{kept->at(x, y, z)} : std::nullopt;
    }

    // What every voxel of the cube `across` wide from (x, y, z) is, where it's all one.
    [[nodiscard]] std::optional<Cube> whole(int x, int y, int z, int across) const {
        const int low = base + z;
        const int high = low + across - 1;
        if (high < bottom || low > top) {
            return Cube{};
        }
        if (low >= keep) {
            return kept_whole(x, y, z, across);
        }
        if (low < bottom || high >= std::min(top + 1, keep)) {
            return std::nullopt;
        }
        const Cube& first = run_at(x, y, low).voxel;
        for (int b = y; b < y + across; ++b) {
            for (int a = x; a < x + across; ++a) {
                const Run& run = run_at(a, b, low);
                if (run.high < high || run.voxel != first) {
                    return std::nullopt;
                }
            }
        }
        return first;
    }

    [[nodiscard]] Cube voxel(int x, int y, int z) const {
        const int layer = base + z;
        if (layer < bottom || layer > top) {
            return {};
        }
        if (layer >= keep) {
            return kept == nullptr ? Cube{} : kept->at(x, y, z);
        }
        return run_at(x, y, layer).voxel;
    }
};

VoxelTree tile_of(const TileSource& source) {
    return VoxelTree::build_down(
        TileSource::side,
        [&](int x, int y, int z, int across) { return source.whole(x, y, z, across); },
        [&](int x, int y, int z) { return source.voxel(x, y, z); });
}

// Each column's runs from `bottom` to `top`, of `layers`.
std::vector<std::vector<Run>> runs_up(const std::vector<Generator::Column>& columns,
                                      const Planet& planet, const Layers& layers, int bottom,
                                      int top, bool caves = true) {
    std::vector<std::vector<Run>> runs;
    runs.reserve(columns.size());
    for (const Generator::Column& each : columns) {
        runs.push_back(runs_of(each, planet, layers, bottom, top, caves));
    }
    return runs;
}

// The tiles up the tile column at (`face`, `i`, `j`), counted in tiles, holding layers `bottom`
// to `top`, from the columns over it. Under `bottom` they hold nothing; from `keep` up they keep
// what the volume holds.
void build(Volume& volume, const Generator& generator,
           const std::vector<Generator::Column>& columns, std::uint8_t face, std::uint32_t i,
           std::uint32_t j, int bottom, int top, int keep) {
    constexpr int side = Volume::tile_side;
    const std::vector<std::vector<Run>> runs =
        runs_up(columns, generator.planet(), generator.layers(), bottom, std::min(top, keep - 1));
    for (int k = bottom / side; k <= top / side; ++k) {
        const Site place{.face = face,
                         .i = i * side,
                         .j = j * side,
                         .layer = static_cast<std::uint32_t>(k * side)};
        volume.put_tile(place, tile_of({.runs = &runs,
                                        .kept = volume.tile_at(place),
                                        .base = k * side,
                                        .bottom = bottom,
                                        .top = top,
                                        .keep = keep}));
    }
}

std::vector<Generator::Column> columns_of(const Generator& generator, std::uint8_t face,
                                          std::uint32_t i, std::uint32_t j) {
    constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);
    return generator.columns({.level = static_cast<std::uint8_t>(generator.layers().level),
                              .face = face,
                              .i = i * side,
                              .j = j * side},
                             Volume::tile_side, voxel_lod(generator));
}

} // namespace

Layers coarser_layers(const Layers& layers, int level) noexcept {
    const int size = 1 << level;
    return {.level = layers.level - level,
            .floor = layers.floor,
            .thickness = layers.thickness * size,
            .count = (layers.count + size - 1) / size};
}

VoxelTree generate_tile(const Generator& generator, TileKey key) {
    return std::move(generate_stack(generator, std::span{&key, 1}).front().tree);
}

std::vector<MadeTile> generate_stack(const Generator& generator, std::span<const TileKey> keys) {
    constexpr int side = Volume::tile_side;
    std::vector<MadeTile> made;
    made.reserve(keys.size());
    if (keys.empty()) {
        return made;
    }
    const TileKey first = keys.front();
    HOG_ASSERT(std::ranges::all_of(keys,
                                   [&](TileKey key) {
                                       return key.level == first.level && key.face == first.face &&
                                              key.i == first.i && key.j == first.j;
                                   }),
               "tiles over one footprint");
    const Layers layers = coarser_layers(generator.layers(), first.level);
    int lowest = layers.count;
    int highest = 0;
    for (const TileKey key : keys) {
        const int bottom = static_cast<int>(key.k) * side;
        if (bottom < layers.count) {
            lowest = std::min(lowest, bottom);
            highest = std::max(highest, std::min(bottom + side - 1, layers.count - 1));
        }
    }
    if (lowest > highest) {
        for (const TileKey key : keys) {
            made.push_back({.key = key});
        }
        return made;
    }
    const std::vector<Generator::Column> columns =
        generator.columns({.level = static_cast<std::uint8_t>(layers.level),
                           .face = first.face,
                           .i = first.i * static_cast<std::uint32_t>(side),
                           .j = first.j * static_cast<std::uint32_t>(side)},
                          side, voxel_lod(generator) * (1 << first.level));
    const std::vector<std::vector<Run>> runs = runs_up(columns, generator.planet(), layers, lowest,
                                                       highest, layers.thickness <= widest_cave);
    // The lowest the ground over the footprint comes, and beside it as steep as it slopes; and
    // where any surface, the ground's or open water's top, may come finer than the columns show.
    double ground = std::numeric_limits<double>::max();
    std::vector<std::pair<double, double>> surfaces;
    surfaces.reserve(columns.size() * 2);
    for (const Generator::Column& column : columns) {
        ground = std::min(ground, column.ground() - (column.slope() * layers.thickness));
        const double near = (column.slope() * layers.thickness) + layers.thickness;
        surfaces.emplace_back(column.ground() - near, column.ground() + near);
        if (column.water() > column.ground()) {
            surfaces.emplace_back(column.water() - layers.thickness,
                                  column.water() + layers.thickness);
        }
    }
    for (const TileKey key : keys) {
        const int bottom = static_cast<int>(key.k) * side;
        if (bottom >= layers.count) {
            made.push_back({.key = key});
            continue;
        }
        const int top = std::min(bottom + side - 1, layers.count - 1);
        bool solid = ground - layers.thickness >= layers.top(top);
        for (const std::vector<Run>& up : runs) {
            for (const Run& run : up) {
                solid = solid && (run.high < bottom || run.low > top ||
                                  run.voxel.solid_amount >= Voxel::full);
            }
        }
        VoxelTree tree = tile_of({.runs = &runs,
                                  .kept = nullptr,
                                  .base = bottom,
                                  .bottom = bottom,
                                  .top = top,
                                  .keep = top + 1});
        const double low = layers.bottom(bottom);
        const double high = layers.top(top);
        const bool surfaced = std::ranges::any_of(surfaces, [&](const auto& surface) {
            return surface.second >= low && surface.first <= high;
        });
        const bool one = tree.leaf_side(0, 0, 0) >= side;
        made.push_back(
            {.key = key, .tree = std::move(tree), .hidden = solid || (one && !surfaced)});
    }
    return made;
}

std::uint32_t tiles_across(const Generator& generator) {
    return std::max((std::uint32_t{1} << generator.layers().level) / Volume::tile_side, 1U);
}

Area around(const Generator& generator, DVec3 direction, std::uint32_t across) {
    const std::uint32_t side = tiles_across(generator);
    across = std::clamp(across, 1U, side);
    const Cell cell = cell_under(direction, generator.layers().level);
    const auto start = [&](std::uint32_t at) {
        const std::uint32_t tile = at / Volume::tile_side;
        return std::min(tile - std::min(tile, across / 2), side - across);
    };
    return {.face = cell.face, .i = start(cell.i), .j = start(cell.j), .across = across};
}

Volume generate_area(const Generator& generator, const Area& area) {
    Volume volume;
    for (std::uint32_t j = area.j; j < area.j + area.across; ++j) {
        for (std::uint32_t i = area.i; i < area.i + area.across; ++i) {
            volume.merge(generate_tiles(generator, area.face, i, j));
        }
    }
    return volume;
}

void MadeWorld::cover(const Area& area) {
    constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);
    for (std::uint32_t j = area.j; j < area.j + area.across; ++j) {
        for (std::uint32_t i = area.i; i < area.i + area.across; ++i) {
            if (!volume_.floor({.face = area.face, .i = i * side, .j = j * side})) {
                volume_.merge(generate_tiles(world_.generator(), area.face, i, j));
            }
        }
    }
}

void MadeWorld::cover(DVec3 direction, std::uint32_t across) {
    cover(around(world_.generator(), direction, across));
}

void MadeWorld::cover_down(DVec3 direction, int layer, std::uint32_t across) {
    cover(direction, across);
    const Area area = around(world_.generator(), direction, across);
    constexpr auto side = static_cast<std::uint32_t>(Volume::tile_side);
    for (std::uint32_t j = area.j; j < area.j + area.across; ++j) {
        for (std::uint32_t i = area.i; i < area.i + area.across; ++i) {
            const Site corner{.face = area.face, .i = i * side, .j = j * side};
            while (std::cmp_greater(volume_.floor(corner).value_or(0), std::max(layer, 0)) &&
                   deepen(volume_, world_.generator(), area.face, i, j)) {
            }
        }
    }
}

Volume generate_tiles(const Generator& generator, std::uint8_t face, std::uint32_t i,
                      std::uint32_t j) {
    const Layers& layers = generator.layers();
    const std::vector<Generator::Column> columns = columns_of(generator, face, i, j);
    double highest = generator.sea_level().value_or(layers.floor);
    double lowest = layers.ceiling();
    for (const Generator::Column& each : columns) {
        // Solid shows in a voxel from half its thickness over, more so across a slope.
        const double over =
            (layers.thickness / 2.0) * std::sqrt(1.0 + (each.slope() * each.slope()));
        highest = std::max(highest, each.top() + over);
        lowest = std::min(lowest, each.ground());
    }
    constexpr int side = Volume::tile_side;
    const int top = std::min(layers.layer_at(highest), layers.count - 1);
    // From the foot of the tile it starts in, so the column's lowest tile is whole.
    const int bottom = (std::max(layers.layer_at(lowest) - made_depth, 0) / side) * side;
    Volume volume;
    build(volume, generator, columns, face, i, j, bottom, top, top + 1);
    return volume;
}

bool deepen(Volume& volume, const Generator& generator, std::uint8_t face, std::uint32_t i,
            std::uint32_t j) {
    const Site corner{.face = face, .i = i * Volume::tile_side, .j = j * Volume::tile_side};
    const std::optional<std::uint32_t> floor = volume.floor(corner);
    if (!floor || *floor == 0) {
        return false;
    }
    const int keep = static_cast<int>(*floor);
    build(volume, generator, columns_of(generator, face, i, j), face, i, j,
          keep - Volume::tile_side, keep - 1, keep);
    return true;
}

} // namespace hog::globe
