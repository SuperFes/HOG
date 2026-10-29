#include <hog/planet/isostasy.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/tectonics.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using hog::planet::crust_heights;
using hog::planet::fixed_one;
using hog::planet::fixed_to_double;
using hog::planet::LevelGrid;
using hog::planet::Tectonics;

namespace {

constexpr double radius = 115'000.0;

// One column of each kind, side by side on a small grid.
std::vector<std::int64_t> heights_of(const std::vector<Tectonics::Column>& kinds) {
    const LevelGrid grid{1, radius};
    std::vector<Tectonics::Column> columns(grid.size(), kinds.front());
    std::ranges::copy(kinds, columns.begin());
    const Tectonics tectonics{grid, {{}}, columns, {}};
    auto heights = crust_heights(tectonics);
    heights.resize(kinds.size());
    return heights;
}

double area_below(const LevelGrid& grid, const std::vector<std::int64_t>& heights,
                  std::int64_t level) {
    double below = 0.0;
    double area = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        area += static_cast<double>(grid.area(cell));
        below += heights[cell] < level ? static_cast<double>(grid.area(cell)) : 0.0;
    }
    return below / area;
}

} // namespace

TEST_CASE("continents float above the ocean floor, which sinks as it ages", "[planet][isostasy]") {
    const auto heights = heights_of(
        {{.crust = Tectonics::Crust::Oceanic, .age = 0.0F},
         {.crust = Tectonics::Crust::Oceanic, .age = 15.0F},
         {.crust = Tectonics::Crust::Oceanic, .age = 60.0F},
         {.crust = Tectonics::Crust::Oceanic, .age = 200.0F},
         {.crust = Tectonics::Crust::Continental, .thickness = Tectonics::continental_thickness}});
    CHECK(heights[0] == 0);
    CHECK(heights[1] < heights[0]);
    CHECK(heights[2] < heights[1]);
    // By the square root of its age, until it's cooled through.
    CHECK(std::abs(fixed_to_double(heights[1]) - (-hog::planet::cooled_depth / 2.0)) < 1.0);
    CHECK(heights[3] == heights[2]);
    CHECK(fixed_to_double(heights[4]) == hog::planet::continent_freeboard);
}

TEST_CASE("thicker crust floats higher", "[planet][isostasy]") {
    const auto heights =
        heights_of({{.crust = Tectonics::Crust::Continental, .thickness = 35.0F},
                    {.crust = Tectonics::Crust::Continental, .thickness = 70.0F},
                    {.crust = Tectonics::Crust::Oceanic, .thickness = 7.0F, .age = 30.0F},
                    {.crust = Tectonics::Crust::Oceanic, .thickness = 15.0F, .age = 30.0F}});
    // Doubled continental crust stands about as high again as a high plateau.
    const double plateau = fixed_to_double(heights[1] - heights[0]);
    CHECK(plateau > 4'500.0);
    CHECK(plateau < 6'000.0);
    CHECK(heights[3] > heights[2]);
}

TEST_CASE("water fills the lowest ground first", "[planet][isostasy]") {
    const LevelGrid grid{3, radius};
    std::vector<std::int64_t> heights(grid.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        heights[cell] = static_cast<std::int64_t>(cell % 100) * 50 * fixed_one;
    }
    const std::int64_t lowest = std::ranges::min(heights);
    CHECK(hog::planet::water_below(grid, heights, lowest) == 0.0);
    const std::int64_t level = 2'000 * fixed_one;
    const double water = hog::planet::water_below(grid, heights, level);
    CHECK(water > hog::planet::water_below(grid, heights, level - fixed_one));
    // The same water finds its way back to the same level.
    CHECK(std::abs(hog::planet::sea_level(grid, heights, water) - level) <= 1);
    CHECK(hog::planet::sea_level(grid, heights, 0.0) == lowest);
}

TEST_CASE("a level can be found covering a share of the planet", "[planet][isostasy]") {
    const LevelGrid grid{4, radius};
    std::vector<std::int64_t> heights(grid.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        heights[cell] = static_cast<std::int64_t>((cell * 7919U) % 1'000U) * fixed_one;
    }
    for (const double fraction : {0.0, 0.3, 0.7, 1.0}) {
        const std::int64_t level = hog::planet::level_covering(grid, heights, fraction);
        CHECK(std::abs(area_below(grid, heights, level) - fraction) < 0.01);
    }
}

TEST_CASE("a new world's water floods its ocean floor and the shelves round its land",
          "[planet][isostasy]") {
    const Tectonics tectonics{
        1, radius, {.level = 5, .continental = hog::planet::continental_share(0.7)}};
    const auto& grid = tectonics.grid();
    const auto heights = crust_heights(tectonics);
    const std::int64_t sea =
        hog::planet::sea_level(grid, heights, hog::planet::starting_water(grid));
    const double covered = area_below(grid, heights, sea);
    CHECK(covered > 0.6);
    CHECK(covered < 0.8);
    double shelves = 0.0;
    double continents = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const Tectonics::Column& column = tectonics.columns()[cell];
        if (column.crust == Tectonics::Crust::Oceanic) {
            // Ridges and all lie deep under the sea.
            REQUIRE(fixed_to_double(sea - heights[cell]) > 1'000.0);
            continue;
        }
        const auto area = static_cast<double>(grid.area(cell));
        continents += area;
        shelves += heights[cell] < sea ? area : 0.0;
    }
    CHECK(shelves / continents > 0.05);
    CHECK(shelves / continents < 0.5);
}

TEST_CASE("the sea holds its water as the plates move", "[planet][isostasy]") {
    Tectonics tectonics{1, radius, {.level = 5, .continental = 0.4}};
    const auto& grid = tectonics.grid();
    const double water = hog::planet::starting_water(grid);
    for (int step = 0; step < 20; ++step) {
        tectonics.step();
    }
    const auto heights = crust_heights(tectonics);
    const std::int64_t sea = hog::planet::sea_level(grid, heights, water);
    CHECK(std::abs(hog::planet::water_below(grid, heights, sea) - water) < 1e-6 * water);
    // Still mostly sea, though the coast has moved and colliding land piled up into less of it.
    const double covered = area_below(grid, heights, sea);
    CHECK(covered > 0.55);
    CHECK(covered < 0.9);
}

TEST_CASE("a world short of water once settled is topped up from space", "[planet][isostasy]") {
    const Tectonics tectonics{1, radius, {.level = 5, .continental = 0.4}};
    const auto& grid = tectonics.grid();
    const auto heights = crust_heights(tectonics);
    const double water = hog::planet::starting_water(grid);
    const double covered = area_below(grid, heights, hog::planet::sea_level(grid, heights, water));
    // Short: enough more to cover the share asked for.
    const double more = covered + 0.1;
    const double topped = hog::planet::topped_up(grid, heights, water, more);
    CHECK(topped > water);
    CHECK(std::abs(area_below(grid, heights, hog::planet::sea_level(grid, heights, topped)) -
                   more) < 0.01);
    // Enough already: left as it is, never drained.
    CHECK(hog::planet::topped_up(grid, heights, water, covered - 0.1) == water);
}

TEST_CASE("a settled world has moved its plates and filled its sea", "[planet][isostasy]") {
    const Tectonics::Settings settings{.level = 5, .continental = 0.4};
    const auto settled = hog::planet::settle(1, radius, settings, 0.7, 10);
    CHECK(settled.tectonics.steps() == 10);
    const auto& grid = settled.tectonics.grid();
    CHECK(settled.heights == crust_heights(settled.tectonics));
    CHECK(area_below(grid, settled.heights, settled.sea_level) >= 0.69);
    // Short of nothing, nothing is added.
    const auto dry = hog::planet::settle(1, radius, settings, 0.0, 10);
    CHECK(dry.sea_level == hog::planet::sea_level(grid, dry.heights,
                                                  hog::planet::starting_water(grid)));
}
