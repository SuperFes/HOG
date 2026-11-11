#include <hog/census/census.hpp>
#include <hog/globe/cell.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::WithinRel;
using hog::census::Census;
using hog::census::count;
using hog::census::Spot;
using hog::globe::Cell;
using hog::globe::Material;

namespace {

constexpr int level = 3;
constexpr double radius = 1'000.0;

// Dry soil everywhere, 10 m over a sea at 0.
std::vector<Spot> dry() {
    return std::vector<Spot>(hog::globe::cells_at(level),
                             Spot{.ground = 10.0, .water = std::nullopt, .top = Material::Soil});
}

Spot& at(std::vector<Spot>& spots, Cell cell) {
    return spots[static_cast<std::size_t>(hog::globe::index_of(cell))];
}

double km2(Cell cell) {
    return hog::globe::solid_angle(cell) * radius * radius / 1e6;
}

} // namespace

TEST_CASE("a census splits the world into land and sea, the land by what tops it", "[census]") {
    std::vector<Spot> spots = dry();
    const Cell sea{.level = level, .face = 0, .i = 0, .j = 0};
    const Cell sand{.level = level, .face = 1, .i = 2, .j = 2};
    at(spots, sea) = {.ground = -30.0, .water = 0.0, .top = Material::Sediment};
    at(spots, sand).top = Material::Sand;
    const Census census = count(level, spots, radius, 0.0);
    CHECK_THAT(census.area, WithinRel(4.0 * std::numbers::pi * radius * radius / 1e6, 1e-9));
    CHECK_THAT(census.sea, WithinRel(km2(sea), 1e-9));
    CHECK_THAT(census.land, WithinRel(census.area - km2(sea), 1e-9));
    CHECK_THAT(census.tops.at(static_cast<std::size_t>(Material::Sand)),
               WithinRel(km2(sand), 1e-9));
    CHECK(census.tops.at(static_cast<std::size_t>(Material::Sediment)) == 0.0);
    CHECK(census.lowest == -30.0);
    CHECK(census.highest == 10.0);
    CHECK(census.lakes.empty());
}

TEST_CASE("a census joins inland water over the cells' sides into lakes", "[census]") {
    std::vector<Spot> spots = dry();
    // One lake across a cube's edge, another apart from it.
    const Cell edge{.level = level, .face = 2, .i = 7, .j = 4};
    const hog::globe::CellNeighbors beside = hog::globe::sides(edge);
    const Cell over =
        *std::ranges::find_if(beside.all(), [&](Cell next) { return next.face != edge.face; });
    REQUIRE(over.face != edge.face);
    at(spots, edge) = {.ground = 4.0, .water = 12.0, .top = Material::Sediment};
    at(spots, over) = {.ground = 9.0, .water = 12.0, .top = Material::Sediment};
    const Cell pond{.level = level, .face = 5, .i = 3, .j = 3};
    at(spots, pond) = {.ground = 9.5, .water = 10.0, .top = Material::Clay};
    const Census census = count(level, spots, radius, 0.0);
    REQUIRE(census.lakes.size() == 2);
    CHECK(census.running.empty());
    const auto& lake = census.lakes.front().area > census.lakes.back().area ? census.lakes.front()
                                                                            : census.lakes.back();
    CHECK_THAT(lake.area, WithinRel(km2(edge) + km2(over), 1e-9));
    CHECK(lake.level == 12.0);
    CHECK(lake.deepest == 8.0);
    CHECK(census.lakes.front().area >= census.lakes.back().area);
    // Lake beds count as no land.
    CHECK_THAT(census.land, WithinRel(census.area - km2(edge) - km2(over) - km2(pond), 1e-9));
}

TEST_CASE("a census tells running water from still by its level falling", "[census]") {
    std::vector<Spot> spots = dry();
    for (std::uint32_t i = 2; i < 6; ++i) {
        const double level_here = 9.0 - static_cast<double>(i);
        at(spots, {.level = level, .face = 3, .i = i, .j = 1}) = {
            .ground = level_here - 1.0, .water = level_here, .top = Material::Gravel};
    }
    const Census census = count(level, spots, radius, 0.0);
    CHECK(census.lakes.empty());
    REQUIRE(census.running.size() == 1);
    CHECK(census.running.front().fall == 3.0);
    CHECK(census.running.front().level == 4.0);
}

TEST_CASE("a census counts water standing at the sea's level as sea", "[census]") {
    std::vector<Spot> spots = dry();
    const Cell cell{.level = level, .face = 4, .i = 1, .j = 6};
    at(spots, cell) = {.ground = -5.0, .water = 0.0, .top = Material::Sand};
    CHECK(count(level, spots, radius, 0.0).lakes.empty());
    // On a world without a sea, all of it is a lake.
    const Census dry_world = count(level, spots, radius, std::nullopt);
    REQUIRE(dry_world.lakes.size() == 1);
    CHECK(dry_world.sea == 0.0);
}

TEST_CASE("a census report lists its lakes and what tops the land", "[census]") {
    std::vector<Spot> spots = dry();
    at(spots, {.level = level, .face = 0, .i = 4, .j = 4}) = {
        .ground = 2.0, .water = 6.0, .top = Material::Sediment};
    const std::string text = hog::census::report(count(level, spots, radius, 0.0));
    CHECK(text.find("still water: 1 bodies") != std::string::npos);
    CHECK(text.find("running water: 0 bodies") != std::string::npos);
    CHECK(text.find("land tops: soil 100.0%") != std::string::npos);
}
