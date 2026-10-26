#include <hog/planet/neighbors.hpp>
#include <hog/planet/places.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/world.hpp>
#include <hog/testing/worlds.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>

using hog::planet::Biome;
using hog::planet::CellId;
using hog::planet::CellState;
using hog::planet::fixed_one;
using hog::planet::Zone;

namespace {

CellState climate(Biome biome, std::int64_t temperature, std::int64_t moisture) {
    CellState state;
    state.biome = biome;
    state.temperature = temperature;
    state.moisture = moisture;
    return state;
}

hog::planet::World& world() {
    static hog::planet::World world{{.seed = 1,
                                     .ocean_fraction = 0.7,
                                     .radius = 115'000.0,
                                     .erosion_cell = hog::testing::coarse_erosion_cell}};
    return world;
}

std::int64_t degrees(std::int64_t value) {
    return value * fixed_one;
}

} // namespace

TEST_CASE("climate zones fall along the biomes' lines", "[planet][places]") {
    using hog::planet::zone_of;
    const std::int64_t wet = fixed_one * 4 / 5;
    const std::int64_t dry = fixed_one / 20;
    CHECK(zone_of(degrees(-20), wet) == Zone::Polar);
    CHECK(zone_of(degrees(0), dry) == Zone::Polar);
    CHECK(zone_of(degrees(0), wet) == Zone::Cold);
    CHECK(zone_of(degrees(12), wet) == Zone::Temperate);
    CHECK(zone_of(degrees(12), dry) == Zone::Arid);
    CHECK(zone_of(degrees(26), wet) == Zone::Tropical);
    CHECK(zone_of(degrees(26), dry) == Zone::Arid);
    CHECK(hog::planet::to_string(Zone::Tropical) == "tropical");
}

TEST_CASE("people can live off open ground, but not polar, desert, peaks or water",
          "[planet][places]") {
    const CellState temperate = climate(Biome::Grassland, degrees(12), fixed_one / 2);
    CHECK(hog::planet::habitable(temperate));
    for (const Biome biome : {Biome::Ocean, Biome::Lake, Biome::Ice, Biome::Alpine}) {
        CellState state = temperate;
        state.biome = biome;
        CHECK_FALSE(hog::planet::habitable(state));
    }
    CHECK_FALSE(hog::planet::habitable(climate(Biome::Desert, degrees(26), fixed_one / 20)));
    CHECK_FALSE(hog::planet::habitable(climate(Biome::Tundra, degrees(-8), fixed_one / 2)));
}

TEST_CASE("the nearest cell that fits is found, nearest first", "[planet][places]") {
    const hog::DVec3 from = hog::planet::direction_at(0.3, -1.2);
    const CellId under = hog::planet::cell_at(from, 4);
    CHECK(hog::planet::nearest_cell(from, 4, [](CellId) { return true; }) == under);
    // Next to it, without it.
    const std::optional<CellId> beside =
        hog::planet::nearest_cell(from, 4, [&](CellId cell) { return cell != under; });
    REQUIRE(beside.has_value());
    const auto around = hog::planet::vertex_neighbors(under).cells();
    CHECK(std::ranges::find(around, beside.value_or(under)) != around.end());
    CHECK_FALSE(hog::planet::nearest_cell(from, 2, [](CellId) { return false; }).has_value());
}

TEST_CASE("the test world has a coast and every climate zone to find", "[planet][places]") {
    const hog::planet::World& planet = world();
    const auto& terrain = planet.geology().terrain();
    const std::optional<CellId> coast =
        hog::planet::nearest_cell(hog::planet::start_direction, 8,
                                  [&](CellId cell) { return hog::planet::coast(terrain, cell); });
    REQUIRE(coast.has_value());
    const CellId shore = coast.value_or(CellId{});
    CHECK(terrain.is_land(shore));
    CHECK(std::ranges::any_of(hog::planet::edge_neighbors(shore),
                              [&](CellId next) { return !terrain.is_land(next); }));
    for (int zone = 0; zone < hog::planet::zone_count; ++zone) {
        CAPTURE(hog::planet::to_string(static_cast<Zone>(zone)));
        CHECK(hog::planet::nearest_cell(hog::planet::start_direction, 7, [&](CellId cell) {
                  const CellState state = planet.state(cell);
                  return state.elevation > 0 &&
                         hog::planet::zone_of(state) == static_cast<Zone>(zone);
              }).has_value());
    }
    CHECK(hog::planet::nearest_cell(hog::planet::start_direction, 7, [&](CellId cell) {
              return hog::planet::habitable(planet.state(cell));
          }).has_value());
}
