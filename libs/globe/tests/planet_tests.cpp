#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <numbers>

using hog::globe::Planet;
using hog::globe::Volatile;

namespace {

// Cold and thin-aired: water lies only as ice.
Planet frozen() {
    Planet planet;
    planet.pressure = 636.0;
    planet.equator_temperature = 224.0;
    planet.pole_temperature = 150.0;
    return planet;
}

// Methane rivers under a thick cold sky, too warm for methane ice.
Planet methane_world() {
    Planet planet;
    planet.pressure = 146'700.0;
    planet.equator_temperature = 94.0;
    planet.pole_temperature = 91.0;
    planet.volatiles = Volatile::Methane;
    return planet;
}

} // namespace

TEST_CASE("a volatile boils as Clausius and Clapeyron say", "[globe][planet]") {
    const auto water = hog::globe::boiling_point(Volatile::Water, 101'325.0);
    CHECK(std::abs(water.value_or(0.0) - 373.15) < 3.0);
    const auto methane = hog::globe::boiling_point(Volatile::Methane, 101'325.0);
    CHECK(std::abs(methane.value_or(0.0) - 111.65) < 2.0);
    CHECK_FALSE(hog::globe::boiling_point(Volatile::Water, 600.0));
    CHECK_FALSE(hog::globe::boiling_point(Volatile::None, 101'325.0));
    CHECK(hog::globe::boiling_point(Volatile::Water, 1e9).value_or(0.0) == 647.096);
}

TEST_CASE("a volatile's vapour is in balance with it as the boiling point says",
          "[globe][planet]") {
    // It boils where its vapour holds the pressure above it.
    const auto held = hog::globe::saturation_pressure(Volatile::Water, 373.15);
    CHECK(std::abs(held.value_or(0.0) - 101'325.0) < 6'000.0);
    CHECK(hog::globe::saturation_pressure(Volatile::Water, 288.0).value_or(0.0) <
          hog::globe::saturation_pressure(Volatile::Water, 300.0).value_or(0.0));
    CHECK_FALSE(hog::globe::saturation_pressure(Volatile::Water, 700.0));
    CHECK_FALSE(hog::globe::saturation_pressure(Volatile::None, 288.0));
    CHECK(hog::globe::latent_heat(Volatile::Water).value_or(0.0) > 2.2e6);
    CHECK(hog::globe::latent_heat(Volatile::Water).value_or(0.0) < 2.6e6);
}

TEST_CASE("liquid and ice stand where the air and warmth let them", "[globe][planet]") {
    const auto mild = hog::globe::derive(Planet{});
    CHECK(mild.liquid);
    CHECK(mild.ice);
    const auto cold = hog::globe::derive(frozen());
    CHECK_FALSE(cold.liquid);
    CHECK(cold.ice);
    Planet dry;
    dry.volatiles = Volatile::None;
    CHECK_FALSE(hog::globe::derive(dry).liquid);
    CHECK_FALSE(hog::globe::derive(dry).ice);

    Planet titan = methane_world();
    const auto alien = hog::globe::derive(titan);
    CHECK(alien.liquid);
    CHECK_FALSE(alien.ice);
    // Hot enough to boil it everywhere.
    titan.pole_temperature = 130.0;
    titan.equator_temperature = 140.0;
    CHECK_FALSE(hog::globe::derive(titan).liquid);
}

TEST_CASE("temperature falls from equator to pole, averaging the measured mean",
          "[globe][planet]") {
    const Planet earth;
    CHECK(hog::globe::temperature_at(earth, 0.0) == earth.equator_temperature);
    CHECK(std::abs(hog::globe::temperature_at(earth, std::numbers::pi / 2.0) -
                   earth.pole_temperature) < 1e-9);
    CHECK(std::abs(hog::globe::mean_temperature(earth) - 288.0) < 2.0);
    CHECK(std::abs(hog::globe::mean_temperature(frozen()) - 208.0) < 2.0);
}

TEST_CASE("the crust's heat follows from the knobs", "[globe][planet]") {
    const Planet earth;
    const auto derived = hog::globe::derive(earth);
    // Continental crust warms 25 to 35 K a kilometre.
    CHECK(derived.geotherm > 0.025);
    CHECK(derived.geotherm < 0.035);
    CHECK(derived.floor_temperature > hog::globe::mean_temperature(earth));
}

TEST_CASE("only a planet that can be is valid", "[globe][planet]") {
    for (const Planet& planet : {Planet{}, frozen(), methane_world()}) {
        CHECK(hog::globe::validate(planet).has_value());
    }
    Planet hollow;
    hollow.floor_depth = hollow.radius;
    CHECK_FALSE(hog::globe::validate(hollow).has_value());
    Planet unknown;
    unknown.gravity = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(hog::globe::validate(unknown).has_value());
    for (const double detail : {0.0, 3.0, 5.5, 10.0}) {
        Planet unformed;
        unformed.formed_detail = detail;
        CHECK_FALSE(hog::globe::validate(unformed).has_value());
    }
}

TEST_CASE("a planet file changes the default knobs by name", "[globe][planet]") {
    const auto read = hog::globe::parse_planet(R"(
[globe]
seed = 7
gravity = 4
volatiles = "methane"
formed_detail = 6
)");
    REQUIRE(read.has_value());
    const Planet& planet = read.value();
    CHECK(planet.seed == 7);
    CHECK(planet.gravity == 4.0);
    CHECK(planet.volatiles == Volatile::Methane);
    CHECK(planet.formed_detail == 6.0);
    CHECK(planet.pressure == Planet{}.pressure);

    const auto plain = hog::globe::parse_planet("[globe]\n");
    REQUIRE(plain.has_value());
    CHECK(plain.value().pressure == Planet{}.pressure);
}

TEST_CASE("a planet file refuses what it doesn't know or can't be", "[globe][planet]") {
    for (const char* text : {
             "",
             "[globe]\npreset = \"earth\"\n",
             "[globe]\ngravty = 3\n",
             "[globe]\ngravity = \"high\"\n",
             "[globe]\ngravity = -1\n",
             "[globe]\nseed = -1\n",
             "[globe]\nvolatiles = \"lava\"\n",
             "[globe]\nwear = 0.1\n",
             "[globe]\nformed_detail = 0\n",
             "[globe]\nhigh = 3\n",
             "[globe\n",
         }) {
        CAPTURE(text);
        CHECK_FALSE(hog::globe::parse_planet(text).has_value());
    }
    CHECK(hog::globe::parse_planet("[globe\n").error().code == hog::ErrorCode::Parse);
    CHECK_FALSE(hog::globe::load_planet("no/such/planet.toml").has_value());
}

TEST_CASE("a world's id tells its knobs apart", "[globe][planet]") {
    const hog::globe::Planet earth;
    hog::globe::Planet other = earth;
    other.seed = 2;
    hog::globe::Planet larger = earth;
    larger.radius = 120'000.0;
    hog::globe::Planet finer = earth;
    finer.formed_detail = 6.0;
    CHECK(hog::globe::world_id(earth) == hog::globe::world_id(hog::globe::Planet{}));
    CHECK(hog::globe::world_id(earth) != hog::globe::world_id(other));
    CHECK(hog::globe::world_id(earth) != hog::globe::world_id(larger));
    CHECK(hog::globe::world_id(earth) != hog::globe::world_id(finer));
    CHECK(hog::globe::world_id(earth) != hog::globe::world_id(frozen()));
}

TEST_CASE("heights keep their proportion to the radius", "[globe][planet]") {
    const Planet full;
    Planet small;
    small.radius = full.radius / 8.0;
    CHECK(hog::globe::derive(full).vertical == 1.0);
    CHECK(std::abs(hog::globe::derive(small).vertical - 0.125) < 1e-12);
}

TEST_CASE("a small world's ground stays inside its shell", "[globe][planet]") {
    Planet small;
    small.radius = 15'000.0;
    small.floor_depth = 500.0;
    small.sky_height = 3'000.0;
    const hog::globe::Generator generator{small};
    for (std::uint64_t k = 0; k < 98'304; k += 997) {
        const auto ground = hog::globe::ground_below(
            generator, hog::globe::center(hog::globe::cell_at_index(7, k)), 20.0);
        REQUIRE(ground.has_value());
        CHECK(*ground - small.radius < small.sky_height);
        CHECK(*ground - small.radius > -small.floor_depth - 1'000.0);
    }
}

TEST_CASE("a planet sent as bytes is the same planet, and a cut or odd one is refused",
          "[globe][planet]") {
    for (const Planet& planet : {Planet{}, frozen(), methane_world()}) {
        const auto bytes = hog::globe::encode_planet(planet);
        const auto read = hog::globe::decode_planet(bytes);
        REQUIRE(read);
        CHECK(hog::globe::world_id(*read) == hog::globe::world_id(planet));
        CHECK(read->seed == planet.seed);
    }
    Planet odd;
    odd.seed = 0xFEED'FACE'1234ULL;
    odd.radius = 12'345.678;
    odd.volatiles = Volatile::Methane;
    odd.heat_flow = 0.02;
    odd.formed_detail = 7.0;
    const auto bytes = hog::globe::encode_planet(odd);
    const auto read = hog::globe::decode_planet(bytes);
    REQUIRE(read);
    CHECK(hog::globe::world_id(*read) == hog::globe::world_id(odd));
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CHECK_FALSE(hog::globe::decode_planet(std::span{bytes}.first(length)));
    }
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(hog::globe::decode_planet(longer));
}
