#include <hog/planet/isostasy.hpp>
#include <hog/planet/lithosphere.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tectonics.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::planet::crust_heights;
using hog::planet::CrustType;
using hog::planet::fixed_one;
using hog::planet::fixed_to_double;
using hog::planet::LevelGrid;
using hog::planet::Lithosphere;
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

// Every cell of the grid, in order.
std::vector<std::uint32_t> every_cell(const LevelGrid& grid) {
    std::vector<std::uint32_t> cells(grid.size());
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        cells[cell] = cell;
    }
    return cells;
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

TEST_CASE("crust the plates carry floats as thick and as old as it is", "[planet][isostasy]") {
    const LevelGrid grid{2, radius};
    Lithosphere::Plate plate;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        plate.cells.push_back(cell);
        plate.crust.emplace_back();
    }
    plate.crust[1].age = 50.0F;
    plate.crust[2].age = 1'000.0F;
    plate.crust[3] = {.type = CrustType::Continental, .thickness = 35.0F, .age = 900.0F};
    plate.crust[4] = {.type = CrustType::Continental, .thickness = 45.0F, .age = 900.0F};
    const auto heights = crust_heights(Lithosphere{grid, {plate}});
    REQUIRE(heights.size() == grid.size());
    CHECK(heights[0] == 0);
    CHECK(heights[1] < 0);
    CHECK(heights[2] < heights[1]);
    CHECK_THAT(fixed_to_double(heights[2]), WithinAbs(-hog::planet::cooled_depth, 1e-3));
    CHECK_THAT(fixed_to_double(heights[3]), WithinAbs(hog::planet::continent_freeboard, 1e-3));
    CHECK(heights[4] > heights[3]);
}

TEST_CASE("where the plates have parted, fresh floor stands at a ridge's crest",
          "[planet][isostasy]") {
    const LevelGrid grid{2, radius};
    Lithosphere::Plate plate;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (hog::planet::cell_center(grid.cell(cell)).z > 0.0) {
            plate.cells.push_back(cell);
            plate.crust.push_back({.type = CrustType::Oceanic, .thickness = 7.0F, .age = 500.0F});
        }
    }
    const auto heights = crust_heights(Lithosphere{grid, {plate}});
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const bool carried = hog::planet::cell_center(grid.cell(cell)).z > 0.0;
        CHECK(heights[cell] == (carried ? heights[plate.cells.front()] : 0));
    }
    CHECK(heights[plate.cells.front()] < 0);
}

TEST_CASE("land wears down by how far it stands over the ground around it", "[planet][isostasy]") {
    using hog::planet::continental_buoyancy;
    using hog::planet::denudation;
    using hog::planet::worn_down;
    // A plateau 2 km over the sea, with one peak a kilometre higher and one place under the sea
    // across the planet from it.
    const LevelGrid grid{2, radius};
    std::vector<std::int64_t> heights(grid.size(), 2'000 * fixed_one);
    constexpr std::uint32_t peak = 0;
    heights[peak] = 3'000 * fixed_one;
    std::uint32_t low = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        if (hog::planet::cell_center(grid.cell(cell)).z <
            hog::planet::cell_center(grid.cell(low)).z) {
            low = cell;
        }
    }
    heights[low] = -100 * fixed_one;
    const std::uint32_t coast = grid.neighbors(low).front();
    const std::uint32_t flat = grid.neighbors(peak).front();
    const std::int64_t sea = 0;
    const std::vector<hog::planet::Cover> bare(grid.size(), hog::planet::Cover{});
    const std::vector<double> floating(grid.size(), continental_buoyancy);
    const auto worn = worn_down(grid, heights, sea, 0.01, bare, floating);
    CHECK(worn[low] == 0.0F);
    CHECK(worn[flat] == 0.0F);
    CHECK_THAT(static_cast<double>(worn[peak]), WithinAbs(denudation * 0.01, 1e-5));
    // Down to the sea, not to the ground under it.
    CHECK_THAT(static_cast<double>(worn[coast]),
               WithinAbs(2.0 * static_cast<double>(worn[peak]), 1e-6));
    // Given long enough, as much as brings it down to the sea, and no more.
    const auto ages = worn_down(grid, heights, sea, 1e6, bare, floating);
    CHECK_THAT(static_cast<double>(ages[coast]), WithinAbs(2.0 / continental_buoyancy, 1e-4));
    // Held down by a stiff plate, it lowers by all that's worn off it, so wears as fast to begin
    // with, but no more than its height.
    const std::vector<double> held(grid.size(), 1.0);
    CHECK_THAT(static_cast<double>(worn_down(grid, heights, sea, 0.01, bare, held)[peak]),
               WithinAbs(static_cast<double>(worn[peak]), 1e-5));
    CHECK_THAT(static_cast<double>(worn_down(grid, heights, sea, 1e6, bare, held)[coast]),
               WithinAbs(2.0, 1e-4));
}

TEST_CASE("each kind of rock wears at its own pace, and then the rock under it as ever",
          "[planet][isostasy]") {
    using hog::planet::continental_buoyancy;
    using hog::planet::denudation;
    using hog::planet::softness;
    using hog::planet::worn_down;
    // A plateau 2 km over the sea, with one peak a kilometre higher.
    const LevelGrid grid{2, radius};
    std::vector<std::int64_t> heights(grid.size(), 2'000 * fixed_one);
    constexpr std::uint32_t peak = 0;
    heights[peak] = 3'000 * fixed_one;
    using hog::planet::Cover;
    const auto worn = [&](Cover soft, double years) {
        std::vector<Cover> cover(grid.size(), Cover{});
        cover[peak] = soft;
        return static_cast<double>(
            worn_down(grid, heights, 0, years, cover,
                      std::vector<double>(grid.size(), continental_buoyancy))[peak]);
    };
    const double rock = worn({}, 0.01);
    CHECK_THAT(worn({5.0F, 0.0F}, 0.01), WithinAbs(softness[0] * rock, 1e-4));
    CHECK_THAT(worn({0.0F, 5.0F}, 0.01), WithinAbs(softness[1] * rock, 1e-4));
    CHECK_THAT(worn({0.0F, 0.0F, 5.0F}, 0.01), WithinAbs(softness[2] * rock, 1e-5));
    CHECK(worn({0.0F, 0.0F, 5.0F}, 0.01) < rock);
    // A thin cover wears off quickly, and then what's under it as it would have.
    const double thin = worn({0.01F, 0.0F}, 10.0);
    CHECK(thin > worn({}, 10.0));
    CHECK(thin < worn({5.0F, 0.0F}, 10.0));
    CHECK(worn({0.01F, 5.0F}, 1.0) > worn({0.0F, 5.0F}, 1.0));
    CHECK(worn({0.01F, 5.0F}, 1.0) < worn({5.0F, 0.0F}, 1.0));
    CHECK_THAT(rock, WithinAbs(denudation * 0.01, 1e-5));
    // Given long enough, as much as brings it down to the ground around, and no more.
    for (const Cover soft : {Cover{}, Cover{0.01F, 0.0F}, Cover{5.0F, 0.0F}, Cover{0.5F, 5.0F},
                             Cover{0.0F, 0.0F, 5.0F}}) {
        CHECK_THAT(worn(soft, 1e6), WithinAbs(1.0 / continental_buoyancy, 1e-4));
    }
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
    CHECK(settled.heights == hog::planet::flexed(grid, crust_heights(settled.tectonics)));
    CHECK(area_below(grid, settled.heights, settled.sea_level) >= 0.69);
    // Short of nothing, nothing is added.
    const auto dry = hog::planet::settle(1, radius, settings, 0.0, 10);
    CHECK(dry.sea_level ==
          hog::planet::sea_level(grid, dry.heights, hog::planet::starting_water(grid)));
}

TEST_CASE("the plates bend under a column standing alone", "[planet][isostasy]") {
    const LevelGrid grid{5, radius};
    std::vector<std::int64_t> heights(grid.size(), 0);
    const std::uint32_t peak = grid.size() / 2;
    heights[peak] = 5'000 * fixed_one;
    const auto bent = hog::planet::flexed(grid, heights);
    // Lowered, its load spread over its neighbours, none of it lost.
    CHECK(bent[peak] < heights[peak] / 2);
    CHECK(bent[peak] > 0);
    for (const std::uint32_t neighbor : grid.neighbors(peak)) {
        CHECK(bent[neighbor] > 0);
        CHECK(bent[neighbor] <= bent[peak]);
    }
    double load = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        load += fixed_to_double(bent[cell]);
    }
    CHECK(std::abs(load - 5'000.0) < 50.0);
    // Flat ground stays flat.
    const std::vector<std::int64_t> flat(grid.size(), 300 * fixed_one);
    const auto still = hog::planet::flexed(grid, flat);
    CHECK(std::ranges::all_of(
        still, [](std::int64_t height) { return std::abs(height - (300 * fixed_one)) <= 1; }));
}

TEST_CASE("plates settle after turning as far, however fine their grid", "[planet][isostasy]") {
    using hog::planet::settling_steps;
    const int coarse = settling_steps({.level = 6});
    CHECK(coarse > 20);
    CHECK(coarse < 40);
    CHECK(std::abs(settling_steps({.level = 7}) - (2 * coarse)) <= 1);
    CHECK(std::abs(settling_steps({.level = 6, .travel = 3.0}) - (coarse / 2)) <= 1);
}

TEST_CASE("a weak plate, or a load as broad as the plate, sinks as far as it would alone",
          "[planet][isostasy]") {
    using hog::planet::deflected;
    const LevelGrid grid{3, radius};
    const std::vector<std::uint32_t> cells = every_cell(grid);
    std::vector<double> loads(grid.size(), 0.0);
    loads[5] = 100.0;
    loads[40] = -30.0;
    const auto alone = deflected(grid, cells, loads, std::vector<double>(grid.size(), 0.0));
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        CHECK_THAT(alone[cell], WithinAbs(loads[cell], 1e-9));
    }
    const auto area = static_cast<double>(grid.area(0));
    const auto broad = deflected(grid, cells, std::vector<double>(grid.size(), 250.0),
                                 std::vector<double>(grid.size(), area * area));
    for (const double sunk : broad) {
        CHECK_THAT(sunk, WithinAbs(250.0, 1e-6));
    }
}

TEST_CASE("a stiff plate spreads a narrow load round it, sinking as much in all",
          "[planet][isostasy]") {
    const LevelGrid grid{4, radius};
    const std::vector<std::uint32_t> cells = every_cell(grid);
    // Mid-face, away from the cube's edges.
    constexpr std::uint32_t at = (16 * 8) + 8;
    std::vector<double> loads(grid.size(), 0.0);
    loads[at] = 1'000.0;
    const auto area = static_cast<double>(grid.area(at));
    const auto bent_by = [&](double stiff) {
        return hog::planet::deflected(grid, cells, loads, std::vector<double>(grid.size(), stiff));
    };
    const auto soft = bent_by(0.1 * area * area);
    const auto hard = bent_by(area * area);
    CHECK(soft[at] < 1'000.0);
    CHECK(hard[at] < soft[at]);
    CHECK(hard[at] > 0.0);
    for (const std::uint32_t next : grid.neighbors(at)) {
        CHECK(hard[next] > 0.0);
        CHECK(hard[next] < hard[at]);
    }
    double sunk = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        sunk += hard[cell] * static_cast<double>(grid.area(cell));
    }
    CHECK_THAT(sunk, WithinRel(1'000.0 * area, 1e-6));
}

TEST_CASE("a plate is broken at its edge, so a load there sinks further than one inside it",
          "[planet][isostasy]") {
    const LevelGrid grid{4, radius};
    // The northern half only.
    std::vector<std::uint32_t> cells;
    std::uint32_t inside = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const double z = hog::planet::cell_center(grid.cell(cell)).z;
        if (z > 0.0) {
            cells.push_back(cell);
        }
        if (z > hog::planet::cell_center(grid.cell(inside)).z) {
            inside = cell;
        }
    }
    const auto on_plate = [&](std::uint32_t cell) {
        return std::ranges::binary_search(cells, cell);
    };
    std::size_t edge = 0;
    while (std::ranges::all_of(grid.neighbors(cells[edge]), on_plate)) {
        ++edge;
    }
    const auto node =
        static_cast<std::size_t>(std::ranges::lower_bound(cells, inside) - cells.begin());
    const auto sinks = [&](std::size_t at) {
        std::vector<double> loads(cells.size(), 0.0);
        loads[at] = 1'000.0;
        const auto area = static_cast<double>(grid.area(cells[at]));
        const auto bent = hog::planet::deflected(grid, cells, loads,
                                                 std::vector<double>(cells.size(), area * area));
        double sunk = 0.0;
        for (std::size_t other = 0; other < cells.size(); ++other) {
            sunk += bent[other] * static_cast<double>(grid.area(cells[other]));
        }
        CHECK_THAT(sunk, WithinRel(1'000.0 * area, 1e-6));
        return bent[at];
    };
    const double at_edge = sinks(edge);
    CHECK(at_edge < 1'000.0);
    CHECK(at_edge > sinks(node));
}

TEST_CASE("plates stiffen as they cool, as deep as the plate under them allows",
          "[planet][isostasy]") {
    using hog::planet::Crust;
    using hog::planet::elastic_thickness;
    using hog::planet::stiffness;
    CHECK(elastic_thickness(Crust{}) == 0.0);
    CHECK_THAT(elastic_thickness(Crust{.age = 100.0F}) / elastic_thickness(Crust{.age = 25.0F}),
               WithinRel(2.0, 1e-6));
    CHECK_THAT(elastic_thickness(Crust{.age = 4'000.0F}),
               WithinRel(hog::planet::oceanic_plate / 3.0, 1e-9));
    // Continents by how long since they were last piled up into a range.
    CHECK(
        elastic_thickness(Crust{
            .type = CrustType::Continental, .thickness = 60.0F, .age = 3'000.0F, .raised = 0.0F}) ==
        0.0);
    CHECK_THAT(elastic_thickness(Crust{.type = CrustType::Continental,
                                       .thickness = 35.0F,
                                       .age = 3'000.0F,
                                       .raised = 3'000.0F}),
               WithinRel(hog::planet::continental_plate / 3.0, 1e-9));
    // On Earth, a plate 30 km thick bends over some 70 to 80 km (Watts 2001).
    constexpr double earth = 6'371'000.0;
    const double reach = std::pow(4.0 * stiffness(30.0, earth), 0.25);
    CHECK(reach > 70e3);
    CHECK(reach < 80e3);
    // Over as much of a smaller planet.
    CHECK_THAT(stiffness(30.0, earth / 2.0), WithinRel(stiffness(30.0, earth) / 16.0, 1e-9));
}

TEST_CASE("a cold plate holds a crater dug into it a hollow, and young floor floats as it would",
          "[planet][isostasy]") {
    using hog::planet::Crust;
    const auto struck = [](const LevelGrid& grid, Crust crust, std::uint32_t at) {
        Lithosphere::Plate plate;
        for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
            plate.cells.push_back(cell);
            plate.crust.push_back(crust);
        }
        Lithosphere lithosphere{grid, {plate}};
        lithosphere.bend(radius);
        const double across = std::sqrt(4.0 * std::numbers::pi / static_cast<double>(grid.size()));
        const hog::planet::Impact impact{.at = hog::planet::cell_center(grid.cell(at)),
                                         .diameter = 5.0 * across};
        lithosphere.strike(std::span{&impact, 1});
        return lithosphere;
    };

    const LevelGrid grid{6, radius};
    const Crust shield{
        .type = CrustType::Continental, .thickness = 35.0F, .age = 3'000.0F, .raised = 3'000.0F};
    // Mid-face, away from the cube's edges.
    constexpr std::uint32_t at = (64 * 32) + 32;
    Lithosphere lithosphere = struck(grid, shield, at);
    // Bent once already, under crust it bore all along.
    for (const Crust& crust : lithosphere.plates()[0].crust) {
        if (crust.thickness == shield.thickness) {
            CHECK(crust.sunk == 0.0F);
        }
    }
    const auto alone = crust_heights(lithosphere);
    lithosphere.bend(radius);
    const auto bent = crust_heights(lithosphere);
    CHECK(bent[at] < alone[at]);
    // The plate round it holds as much up as the hollow holds down.
    double sunk = 0.0;
    double moved = 0.0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const auto area = static_cast<double>(grid.area(cell));
        const auto by = static_cast<double>(lithosphere.plates()[0].crust[cell].sunk);
        sunk += area * by;
        moved += area * std::abs(by);
        CHECK(lithosphere.plates()[0].crust[cell].borne ==
              lithosphere.plates()[0].crust[cell].thickness);
    }
    CHECK(moved > 0.0);
    CHECK(std::abs(sunk) < 1e-4 * moved);
    // Bent again under nothing new, it stays as it is.
    lithosphere.bend(radius);
    CHECK(crust_heights(lithosphere) == bent);

    const LevelGrid coarse{4, radius};
    Lithosphere floor = struck(coarse, Crust{}, (16 * 8) + 8);
    const auto floating = crust_heights(floor);
    floor.bend(radius);
    CHECK(crust_heights(floor) == floating);
}

TEST_CASE("a plate lets what it held up float once heated through, taken off, or gone",
          "[planet][isostasy]") {
    using hog::planet::Crust;
    const LevelGrid grid{5, radius};
    constexpr std::uint32_t at = (32 * 16) + 16;
    const Crust shield{
        .type = CrustType::Continental, .thickness = 35.0F, .age = 3'000.0F, .raised = 3'000.0F};
    Lithosphere::Plate plate;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        plate.cells.push_back(cell);
        plate.crust.push_back(shield);
    }
    // The plate as it stands, changed by `change`, and bent.
    const auto bent = [&](const Lithosphere& lithosphere, const auto& change) {
        std::vector<Lithosphere::Plate> plates(lithosphere.plates().begin(),
                                               lithosphere.plates().end());
        for (Crust& crust : plates[0].crust) {
            change(crust);
        }
        Lithosphere changed{grid, std::move(plates)};
        changed.bend(radius);
        return changed;
    };
    const auto floating = [](const Lithosphere& lithosphere) {
        return std::ranges::all_of(lithosphere.plates()[0].crust,
                                   [](const Crust& crust) { return std::abs(crust.sunk) < 1e-4F; });
    };
    const Lithosphere cold = bent(Lithosphere{grid, {plate}}, [](Crust&) {});
    std::vector<Lithosphere::Plate> lumped(cold.plates().begin(), cold.plates().end());
    lumped[0].crust[at].thickness = 50.0F;
    Lithosphere loaded{grid, std::move(lumped)};
    loaded.bend(radius);
    REQUIRE(loaded.plates()[0].crust[at].sunk < -1.0F);

    // Heated through, it floats, and cooled again it holds up none of what floated while hot.
    const Lithosphere heated = bent(loaded, [](Crust& crust) { crust.raised = 0.0F; });
    CHECK(floating(heated));
    CHECK(floating(bent(heated, [](Crust& crust) { crust.raised = 3'000.0F; })));

    // Taken off again once the plate's weaker, but not too hot to hold anything, the ground's as
    // it was.
    const Lithosphere worn = bent(loaded, [](Crust& crust) {
        crust.raised = 100.0F;
        crust.thickness = 35.0F;
    });
    CHECK(floating(worn));

    // Gone from the plate, it takes what it bent with it.
    std::vector<Lithosphere::Plate> parted(loaded.plates().begin(), loaded.plates().end());
    parted[0].cells.erase(parted[0].cells.begin() + at);
    parted[0].crust.erase(parted[0].crust.begin() + at);
    Lithosphere left{grid, std::move(parted)};
    left.bend(radius);
    CHECK(floating(left));
}

TEST_CASE("worn land rises again by its buoyancy where its plate is weak, and less where stiff",
          "[planet][isostasy]") {
    using hog::planet::continental_buoyancy;
    using hog::planet::Crust;
    const auto lowered = [](Crust crust) {
        const LevelGrid grid{6, radius};
        Lithosphere::Plate plate;
        for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
            plate.cells.push_back(cell);
            plate.crust.push_back(crust);
        }
        return Lithosphere{grid, {plate}}.lowering(radius);
    };
    const auto weak = lowered(
        Crust{.type = CrustType::Continental, .thickness = 40.0F, .age = 1'000.0F, .raised = 0.0F});
    CHECK(std::ranges::all_of(
        weak, [](double lowers) { return std::abs(lowers - continental_buoyancy) < 1e-12; }));
    const auto stiff = lowered(Crust{
        .type = CrustType::Continental, .thickness = 40.0F, .age = 1'000.0F, .raised = 1'000.0F});
    CHECK(std::ranges::all_of(
        stiff, [](double lowers) { return lowers > continental_buoyancy + 0.1 && lowers < 1.0; }));
}

TEST_CASE("lower crust flows from under high ground to under low, keeping its volume",
          "[planet][isostasy]") {
    using hog::planet::flowed;
    const LevelGrid grid{4, radius};
    const std::vector<std::uint32_t> cells = every_cell(grid);
    // Mid-face, away from the cube's edges.
    constexpr std::uint32_t at = (16 * 8) + 8;
    std::vector<double> thickness(grid.size(), 35.0);
    thickness[at] = 45.0;
    const std::vector<double> level(grid.size(), 0.0);
    const std::vector<double> floating(grid.size(), hog::planet::continental_buoyancy);
    const auto volume = [&](const std::vector<double>& thick) {
        double total = 0.0;
        for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
            total += thick[cell] * static_cast<double>(grid.area(cell));
        }
        return total;
    };
    const auto area = static_cast<double>(grid.area(at));
    const auto after = [&](double ease, double years, const std::vector<double>& sunk) {
        return flowed(grid, cells, thickness, sunk, floating,
                      std::vector<double>(grid.size(), ease), years);
    };
    CHECK(after(0.0, 10.0, level) == thickness);
    const auto spread = after(area, 1.0, level);
    CHECK_THAT(volume(spread), WithinRel(volume(thickness), 1e-9));
    CHECK(spread[at] < 45.0);
    for (const std::uint32_t next : grid.neighbors(at)) {
        CHECK(spread[next] > 35.0);
        CHECK(spread[next] < spread[at]);
    }
    // In time, flat.
    const auto flat = after(area, 1e9, level);
    const double mean = volume(thickness) / volume(std::vector<double>(grid.size(), 1.0));
    for (const double thick : flat) {
        CHECK_THAT(thick, WithinAbs(mean, 1e-3));
    }
    // Ground sunk lower draws it in though it's no thinner.
    const std::vector<double> even(grid.size(), 35.0);
    std::vector<double> sunk(grid.size(), 0.0);
    sunk[at] = 1.0;
    const auto drawn =
        flowed(grid, cells, even, sunk, floating, std::vector<double>(grid.size(), area), 1.0);
    CHECK(drawn[at] > 35.0);
    CHECK_THAT(volume(drawn), WithinRel(volume(even), 1e-9));
    // In time, as much as levels the ground, more where it rises less for it.
    const auto levelled = [&](double lowers) {
        const auto flat = flowed(grid, cells, even, sunk, std::vector<double>(grid.size(), lowers),
                                 std::vector<double>(grid.size(), area), 1e9);
        return flat[at] - flat[grid.neighbors(at).front()];
    };
    CHECK_THAT(levelled(1.0), WithinAbs(1.0, 1e-4));
    CHECK_THAT(levelled(hog::planet::continental_buoyancy),
               WithinAbs(1.0 / hog::planet::continental_buoyancy, 1e-3));
}

TEST_CASE("the base of the crust is hot where it was heated lately, and cold under old shields",
          "[planet][isostasy]") {
    using hog::planet::Crust;
    using hog::planet::moho_temperature;
    const auto at = [](float raised) {
        return moho_temperature(Crust{
            .type = CrustType::Continental, .thickness = 35.0F, .age = 3'000.0F, .raised = raised});
    };
    CHECK_THAT(at(0.0F), WithinAbs(273.15 + hog::planet::mantle_celsius, 1e-9));
    CHECK(at(10.0F) < at(0.0F));
    CHECK(at(100.0F) < at(10.0F));
    CHECK(at(3'000.0F) < at(100.0F));
    // A shield's Moho some 200 to 300 C (Jaupart & Mareschal 1999).
    CHECK(at(3'000.0F) > 273.15 + 200.0);
    CHECK(at(3'000.0F) < 273.15 + 300.0);
}

TEST_CASE("a range freshly piled spreads out under itself, and an old shield's lump stays",
          "[planet][isostasy]") {
    using hog::planet::Crust;
    const LevelGrid grid{5, radius};
    constexpr std::uint32_t at = (32 * 16) + 16;
    const auto crept = [&](float raised) {
        Lithosphere::Plate plate;
        for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
            plate.cells.push_back(cell);
            plate.crust.push_back({.type = CrustType::Continental,
                                   .thickness = cell == at ? 50.0F : 35.0F,
                                   .age = 3'000.0F,
                                   .raised = raised});
        }
        Lithosphere lithosphere{grid, {plate}};
        const auto volume = [&] {
            double total = 0.0;
            for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
                total += static_cast<double>(grid.area(cell)) *
                         static_cast<double>(lithosphere.plates()[0].crust[cell].thickness);
            }
            return total;
        };
        const double before = volume();
        lithosphere.creep(10.0, radius);
        CHECK_THAT(volume(), WithinRel(before, 1e-5));
        return static_cast<double>(lithosphere.plates()[0].crust[at].thickness);
    };
    CHECK(crept(0.0F) < 40.0);
    CHECK_THAT(crept(3'000.0F), WithinAbs(50.0, 1e-3));
}

TEST_CASE("lower crust levels the ground a stiff plate shows, not the crust's thickness",
          "[planet][isostasy]") {
    const LevelGrid grid{5, radius};
    constexpr std::uint32_t at = (32 * 16) + 16;
    Lithosphere::Plate plate;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        plate.cells.push_back(cell);
        plate.crust.push_back({.type = CrustType::Continental,
                               .thickness = cell == at ? 50.0F : 35.0F,
                               .age = 3'000.0F,
                               .raised = 100.0F});
    }
    Lithosphere lithosphere{grid, {plate}};
    lithosphere.bend(radius);
    const std::uint32_t beside = grid.neighbors(at).front();
    const auto above = [&] {
        const auto heights = crust_heights(lithosphere);
        return hog::planet::fixed_to_double(heights[at] - heights[beside]);
    };
    const double lump = above();
    REQUIRE(lump > 0.0);
    lithosphere.creep(1e5, radius);
    lithosphere.bend(radius);
    CAPTURE(lump, above(), lithosphere.plates()[0].crust[at].thickness);
    CHECK(std::abs(above()) < 0.1 * lump);
    // Held up by its plate, the ground's level over crust still thicker than round it.
    CHECK(lithosphere.plates()[0].crust[at].thickness > 36.0F);
}

TEST_CASE("lower crust creeps no thinner than the brittle rock over it", "[planet][isostasy]") {
    using hog::planet::Crust;
    // A warm continent, a little thicker than its brittle lid, with ground held down deep in one
    // place drawing its lower crust in from all round.
    const LevelGrid grid{5, radius};
    constexpr std::uint32_t at = (32 * 16) + 16;
    Lithosphere::Plate plate;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        plate.cells.push_back(cell);
        plate.crust.push_back({.type = CrustType::Continental, .thickness = 20.0F});
        plate.crust.back().sunk = cell == at ? 40.0F : 0.0F;
    }
    Lithosphere lithosphere{grid, {plate}};
    const hog::planet::LowerCrust lower;
    lithosphere.creep(10.0, radius, lower);
    const Lithosphere::Plate& crept = lithosphere.plates()[0];
    CHECK(crept.crust[at].thickness > 20.0F);
    for (const Crust& crust : crept.crust) {
        CHECK(static_cast<double>(crust.thickness) >= lower.brittle - 1e-4);
    }
}
