#include <hog/core/assert.hpp>
#include <hog/planet/erosion.hpp>
#include <hog/planet/noise.hpp>

#include <cmath>
#include <cstddef>

namespace hog::planet {

Drainage erode(const LevelGrid& grid, std::vector<std::int64_t>& heights, const Erosion& erosion,
               std::int64_t sea_level) {
    const std::uint32_t count = grid.size();
    HOG_ASSERT(heights.size() == count && erosion.uplift.size() == count &&
                   erosion.erodibility.size() == count && erosion.runoff.size() == count,
               "one value per cell");
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (heights[cell] > sea_level) {
            heights[cell] += erosion.uplift[cell];
        }
    }
    Drainage drainage = drain(grid, heights, sea_level);

    std::vector<std::int64_t> rain(count);
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        rain[cell] = (grid.area(cell) * erosion.runoff[cell]) >> fixed_fraction_bits;
    }
    const std::vector<std::int64_t> drained = accumulate(drainage, rain);

    for (const std::uint32_t cell : drainage.order) {
        const std::uint32_t receiver = drainage.receiver[cell];
        if (receiver == cell) {
            continue;
        }
        const bool to_sea =
            drainage.receiver[receiver] == receiver && heights[receiver] <= sea_level;
        const std::int64_t base = to_sea ? sea_level : heights[receiver];
        if (heights[cell] <= base) {
            continue;
        }
        const auto neighbors = grid.neighbors(cell);
        std::size_t k = 0;
        while (neighbors[k] != receiver) {
            ++k;
        }
        const auto length = static_cast<double>(grid.distance(cell, k));
        const double factor = static_cast<double>(erosion.erodibility[cell]) *
                              std::sqrt(static_cast<double>(drained[cell])) / length;
        const double height =
            (static_cast<double>(heights[cell]) + (factor * static_cast<double>(base))) /
            (1.0 + factor);
        heights[cell] = std::llround(height);
    }
    return drainage;
}

} // namespace hog::planet
