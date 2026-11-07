#include <hog/globe/volume_build.hpp>

#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
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

// The column's voxels up to `top` as runs: one sample for each stretch nothing changes in, and
// one for each voxel where something may.
std::vector<Run> runs_of(const Generator::Column& column, const Layers& layers, int top) {
    // Layers that may change, with whether a cave may be in them.
    struct Changing {
        int low = 0;
        int high = 0;
        bool cave = false;
    };
    std::vector<Changing> changing;
    for (const Generator::Column::Change& change :
         column.changes(layers.floor, layers.top(top), 0.0)) {
        const int low = std::max(layers.layer_at(change.bottom), 0);
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
    int next = 0;
    for (const Changing& stretch : changing) {
        if (next < stretch.low) {
            add(next, stretch.low - 1, made(column, layers, (next + stretch.low - 1) / 2, false));
        }
        for (int layer = std::max(stretch.low, next); layer <= stretch.high; ++layer) {
            add(layer, layer, made(column, layers, layer, stretch.cave));
        }
        next = std::max(next, stretch.high + 1);
    }
    if (next <= top) {
        add(next, top, made(column, layers, (next + top) / 2, false));
    }
    return runs;
}

} // namespace

Volume generate_tiles(const Generator& generator, std::uint8_t face, std::uint32_t i,
                      std::uint32_t j) {
    constexpr int side = Volume::tile_side;
    const Layers& layers = generator.layers();
    const std::vector<Generator::Column> columns = generator.columns(
        {.level = static_cast<std::uint8_t>(layers.level),
         .face = face,
         .i = i * side,
         .j = j * side},
        side, voxel_lod(generator));
    double highest = generator.sea_level().value_or(layers.floor);
    for (const Generator::Column& each : columns) {
        highest = std::max(highest, each.top());
    }
    const int top = std::min(layers.layer_at(highest), layers.count - 1);
    std::vector<std::vector<Run>> runs;
    runs.reserve(columns.size());
    for (const Generator::Column& each : columns) {
        runs.push_back(runs_of(each, layers, top));
    }
    // The voxel of the column at (x, y) in `layer`, and the highest layer it runs on to.
    const auto at = [&](int x, int y, int layer) -> const Run& {
        const std::vector<Run>& up =
            runs[(static_cast<std::size_t>(y) * side) + static_cast<std::size_t>(x)];
        return *std::ranges::lower_bound(up, layer, {}, &Run::high);
    };
    Volume volume;
    for (int k = 0; k * side <= top; ++k) {
        const auto whole = [&](int x, int y, int z, int across) -> std::optional<Cube> {
            const int low = (k * side) + z;
            const int high = low + across - 1;
            if (low > top) {
                return Cube{};
            }
            if (high > top) {
                return std::nullopt;
            }
            const Cube& first = at(x, y, low).voxel;
            for (int b = y; b < y + across; ++b) {
                for (int a = x; a < x + across; ++a) {
                    const Run& run = at(a, b, low);
                    if (run.high < high || run.voxel != first) {
                        return std::nullopt;
                    }
                }
            }
            return first;
        };
        const auto voxel_at = [&](int x, int y, int z) {
            const int layer = (k * side) + z;
            return layer > top ? Cube{} : at(x, y, layer).voxel;
        };
        volume.put_tile({.face = face,
                         .i = i * side,
                         .j = j * side,
                         .layer = static_cast<std::uint32_t>(k * side)},
                        VoxelTree::build_down(side, whole, voxel_at));
    }
    return volume;
}

} // namespace hog::globe
