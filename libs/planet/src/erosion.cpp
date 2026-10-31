#include <hog/core/assert.hpp>
#include <hog/planet/erosion.hpp>
#include <hog/planet/noise.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace hog::planet {

namespace {

// Carries what rivers wore off `worn_from` down to `heights` downstream, laying it in the first
// lake it reaches, cell by cell from where it flows in, up to the lake's level; what reaches the
// sea is lost to it.
void fill_lakes(const LevelGrid& grid, std::vector<std::int64_t>& heights,
                std::span<const std::int64_t> worn_from, const Drainage& drainage,
                std::int64_t sea_level) {
    // Cubic metres on their way down.
    std::vector<double> carried(grid.size(), 0.0);
    for (auto at = drainage.order.rbegin(); at != drainage.order.rend(); ++at) {
        const std::uint32_t cell = *at;
        const auto area = static_cast<double>(grid.area(cell));
        carried[cell] +=
            area * fixed_to_double(std::max<std::int64_t>(worn_from[cell] - heights[cell], 0));
        const std::int64_t surface = drainage.surface[cell];
        const bool sea = surface == sea_level && heights[cell] <= sea_level;
        if (sea) {
            continue;
        }
        // Water stood over it before the rivers cut, so it lies in a lake.
        if (surface > worn_from[cell] && carried[cell] > 0.0) {
            const double room = area * fixed_to_double(surface - heights[cell]);
            const double laid = std::min(carried[cell], room);
            heights[cell] =
                laid == room
                    ? surface
                    : heights[cell] + std::llround(laid / area * static_cast<double>(fixed_one));
            carried[cell] -= laid;
        }
        const std::uint32_t receiver = drainage.receiver[cell];
        if (receiver != cell) {
            carried[receiver] += carried[cell];
        }
    }
}

} // namespace

Drainage erode(const LevelGrid& grid, std::vector<std::int64_t>& heights, const Erosion& erosion,
               std::int64_t sea_level) {
    const std::uint32_t count = grid.size();
    HOG_ASSERT(heights.size() == count && erosion.uplift.size() == count &&
                   erosion.erodibility.size() == count && erosion.runoff.size() == count &&
                   (erosion.risen.empty() || erosion.risen.size() == count),
               "one value per cell");
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (heights[cell] > sea_level || erosion.seabed_rises) {
            heights[cell] += erosion.uplift[cell];
            if (!erosion.risen.empty()) {
                erosion.risen[cell] += erosion.uplift[cell];
            }
        }
    }
    Drainage drainage = drain(grid, heights, sea_level, erosion.wander);

    std::vector<std::int64_t> rain(count);
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        rain[cell] = (grid.area(cell) * erosion.runoff[cell]) >> fixed_fraction_bits;
    }
    const std::vector<std::int64_t> drained = accumulate(drainage, rain);

    const std::vector<std::int64_t> risen_to =
        erosion.fills_basins ? heights : std::vector<std::int64_t>{};
    for (const std::uint32_t cell : drainage.order) {
        const std::uint32_t receiver = drainage.receiver[cell];
        if (receiver == cell) {
            continue;
        }
        // Rivers cut down to the water they run into, a lake's or the sea's, or the ground.
        const std::int64_t base = drainage.surface[receiver];
        if (heights[cell] <= base) {
            continue;
        }
        const auto neighbors = grid.neighbors(cell);
        std::size_t k = 0;
        while (neighbors[k] != receiver) {
            ++k;
        }
        const auto length = static_cast<double>(grid.distance(cell, k));
        std::int64_t erodibility = erosion.erodibility[cell];
        if (erosion.yielding) {
            const std::int64_t risen = erosion.risen.empty() ? 0 : erosion.risen[cell];
            erodibility = (erodibility * erosion.yielding(cell, heights[cell] - risen)) >>
                          fixed_fraction_bits;
        }
        const double factor = static_cast<double>(erodibility) *
                              std::sqrt(static_cast<double>(drained[cell])) / length;
        const double height =
            (static_cast<double>(heights[cell]) + (factor * static_cast<double>(base))) /
            (1.0 + factor);
        heights[cell] = std::llround(height);
    }
    if (erosion.fills_basins) {
        fill_lakes(grid, heights, risen_to, drainage, sea_level);
    }
    return drainage;
}

} // namespace hog::planet
