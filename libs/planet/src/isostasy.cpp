#include <hog/core/assert.hpp>
#include <hog/planet/isostasy.hpp>
#include <hog/planet/noise.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace hog::planet {

namespace {

[[nodiscard]] std::int64_t to_fixed(double metres) {
    return std::llround(metres * static_cast<double>(fixed_one));
}

[[nodiscard]] double planet_area(const LevelGrid& grid) {
    double area = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        area += static_cast<double>(grid.area(cell));
    }
    return area;
}

} // namespace

std::vector<std::int64_t> crust_heights(const Tectonics& tectonics) {
    constexpr double metres_per_km = 1'000.0;
    const auto columns = tectonics.columns();
    std::vector<std::int64_t> heights(columns.size());
    std::ranges::transform(columns, heights.begin(), [](const Tectonics::Column& column) {
        const auto thickness = static_cast<double>(column.thickness);
        if (column.crust == Tectonics::Crust::Continental) {
            return to_fixed(continent_freeboard +
                            (continental_buoyancy * metres_per_km *
                             (thickness - static_cast<double>(Tectonics::continental_thickness))));
        }
        const double cooled = std::min(static_cast<double>(column.age) / cooling_steps, 1.0);
        return to_fixed((-cooled_depth * std::sqrt(cooled)) +
                        (oceanic_buoyancy * metres_per_km *
                         (thickness - static_cast<double>(Tectonics::oceanic_thickness))));
    });
    return heights;
}

double water_below(const LevelGrid& grid, std::span<const std::int64_t> heights,
                   std::int64_t level) {
    HOG_ASSERT(heights.size() == grid.size(), "one height to each cell");
    double water = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (heights[cell] < level) {
            water += static_cast<double>(grid.area(cell)) * fixed_to_double(level - heights[cell]);
        }
    }
    return water;
}

std::int64_t sea_level(const LevelGrid& grid, std::span<const std::int64_t> heights, double water) {
    HOG_ASSERT(!heights.empty(), "ground to fill");
    // Water deep enough to cover the highest ground evenly is enough to fill any of it.
    std::int64_t low = std::ranges::min(heights);
    std::int64_t high = std::ranges::max(heights) + to_fixed(water / planet_area(grid)) + 1;
    while (low < high) {
        const std::int64_t middle = low + ((high - low) / 2);
        if (water_below(grid, heights, middle) < water) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

std::int64_t level_covering(const LevelGrid& grid, std::span<const std::int64_t> heights,
                            double fraction) {
    HOG_ASSERT(heights.size() == grid.size(), "one height to each cell");
    HOG_ASSERT(fraction >= 0.0 && fraction <= 1.0, "fraction out of range");
    std::vector<std::uint32_t> order(grid.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        order[cell] = cell;
    }
    std::ranges::sort(order, [&](std::uint32_t one, std::uint32_t other) {
        return heights[one] != heights[other] ? heights[one] < heights[other] : one < other;
    });
    const double covered = fraction * planet_area(grid);
    double below = 0.0;
    for (const std::uint32_t cell : order) {
        below += static_cast<double>(grid.area(cell));
        if (below > covered) {
            return heights[cell];
        }
    }
    return heights[order.back()] + 1;
}

double starting_water(const LevelGrid& grid) {
    return water_layer * planet_area(grid);
}

double topped_up(const LevelGrid& grid, std::span<const std::int64_t> heights, double water,
                 double ocean_fraction) {
    return std::max(water,
                    water_below(grid, heights, level_covering(grid, heights, ocean_fraction)));
}

Settled settle(std::uint64_t seed, double radius, Tectonics::Settings settings,
               double ocean_fraction, int steps) {
    Settled settled{.tectonics = Tectonics{seed, radius, settings}, .heights = {}};
    for (int step = 0; step < steps; ++step) {
        settled.tectonics.step();
    }
    const LevelGrid& grid = settled.tectonics.grid();
    settled.heights = crust_heights(settled.tectonics);
    settled.sea_level =
        sea_level(grid, settled.heights,
                  topped_up(grid, settled.heights, starting_water(grid), ocean_fraction));
    return settled;
}

double continental_share(double ocean_fraction) {
    return std::clamp(1.0 - ocean_fraction + shelf_share, 0.05, 0.95);
}

} // namespace hog::planet
