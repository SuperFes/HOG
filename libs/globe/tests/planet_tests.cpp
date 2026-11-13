#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>

using hog::globe::Planet;

namespace {

// Far from a dim star under thin air: water lies only as ice.
Planet frozen() {
    Planet planet;
    planet.system.air.pressure = 636.0;
    planet.system.stars.front().luminosity = 0.3;
    return planet;
}

} // namespace

TEST_CASE("water boils as Clausius and Clapeyron say", "[globe][planet]") {
    const auto water = hog::globe::boiling_point(101'325.0);
    CHECK(std::abs(water.value_or(0.0) - 373.15) < 3.0);
    CHECK_FALSE(hog::globe::boiling_point(600.0));
    CHECK(hog::globe::boiling_point(1e9).value_or(0.0) == 647.096);
}

TEST_CASE("water's vapour is in balance with it as the boiling point says", "[globe][planet]") {
    // It boils where its vapour holds the pressure above it.
    const auto held = hog::globe::saturation_pressure(373.15);
    CHECK(std::abs(held.value_or(0.0) - 101'325.0) < 6'000.0);
    CHECK(hog::globe::saturation_pressure(288.0).value_or(0.0) <
          hog::globe::saturation_pressure(300.0).value_or(0.0));
    CHECK_FALSE(hog::globe::saturation_pressure(700.0));
    CHECK(hog::globe::latent_heat > 2.2e6);
    CHECK(hog::globe::latent_heat < 2.6e6);
}

TEST_CASE("water and ice stand where the air and warmth let them", "[globe][planet]") {
    const auto mild = hog::globe::derive(Planet{});
    CHECK(mild.liquid);
    CHECK(mild.ice);
    const auto cold = hog::globe::derive(frozen());
    CHECK_FALSE(cold.liquid);
    CHECK(cold.ice);
}

TEST_CASE("the climate's temperatures are the formation's, warmest at the equator",
          "[globe][planet]") {
    const Planet earth;
    const hog::formation::Body body = hog::globe::formation_body(earth);
    CHECK(std::abs(hog::globe::temperature_at(earth, 0.3) -
                   (hog::formation::sea_level_temperature(body, 0.3) + 273.15)) < 1e-6);
    CHECK(hog::globe::temperature_at(earth, 0.0) >
          hog::globe::temperature_at(earth, std::numbers::pi / 2.0));
    CHECK(std::abs(hog::globe::mean_temperature(earth) - 288.0) < 10.0);
    CHECK(hog::globe::mean_temperature(frozen()) < hog::globe::mean_temperature(earth) - 30.0);
}

TEST_CASE("the stars set the starlight the land formed under", "[globe][planet]") {
    Planet planet;
    CHECK(std::abs(hog::globe::star_flux(planet.system) - 1'361.0) < 1.0);
    planet.system.stars.push_back(planet.system.stars.front());
    planet.system.orbit.distance *= 2.0;
    CHECK(std::abs(hog::globe::star_flux(planet.system) - (1'361.0 / 2.0)) < 1.0);
    CHECK(hog::globe::formation_body(planet).star_flux == hog::globe::star_flux(planet.system));
}

TEST_CASE("the day is one knob, for the land's climate and the game's clock", "[globe][planet]") {
    Planet planet;
    CHECK(hog::globe::day_length(planet) == std::chrono::hours{4});
    planet.body.day = 6.5;
    CHECK(hog::globe::day_length(planet) == std::chrono::minutes{390});
    CHECK(hog::globe::formation_body(planet).day == 6.5);
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
    for (const Planet& planet : {Planet{}, frozen()}) {
        CHECK(hog::globe::validate(planet).has_value());
    }
    Planet hollow;
    hollow.floor_depth = hollow.body.radius;
    CHECK_FALSE(hog::globe::validate(hollow).has_value());
    Planet unknown;
    unknown.body.gravity = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(hog::globe::validate(unknown).has_value());
    Planet strange;
    strange.body.erodibility = std::numeric_limits<double>::infinity();
    CHECK_FALSE(hog::globe::validate(strange).has_value());
    for (const double detail : {0.0, 3.0, 5.5, 10.0}) {
        Planet unformed;
        unformed.formed_detail = detail;
        CHECK_FALSE(hog::globe::validate(unformed).has_value());
    }
}

TEST_CASE("a planet file sets any knob by name, all in [planet]", "[globe][planet]") {
    const auto read = hog::globe::parse_planet(R"(
[planet]
seed = 7
gravity = 4
day = 10
plates = 9
lid_thickness = 350
formed_detail = 6
voxel = 0.5
moons = []

[planet.ore_abundance]
gold = 2.5

[planet.air]
pressure_kpa = 90
)");
    REQUIRE(read.has_value());
    const Planet& planet = read.value();
    CHECK(planet.body.seed == 7);
    CHECK(planet.body.gravity == 4.0);
    CHECK(planet.body.day == 10.0);
    CHECK(planet.body.plates == 9);
    CHECK(planet.body.lid_thickness == 350.0);
    CHECK(planet.body.ore_abundance.gold == 2.5);
    CHECK(planet.body.ore_abundance.coal == 1.0);
    CHECK(planet.formed_detail == 6.0);
    CHECK(planet.voxel == 0.5);
    CHECK(planet.system.moons.empty());
    CHECK(std::abs(planet.system.air.pressure - 90'000.0) < 1e-9);
    // What isn't given is the working world's.
    CHECK(planet.body.radius == Planet{}.body.radius);
    CHECK(planet.body.tilt == Planet{}.body.tilt);

    const auto plain = hog::globe::parse_planet("[planet]\n");
    REQUIRE(plain.has_value());
    CHECK(hog::globe::world_id(plain.value()) == hog::globe::world_id(Planet{}));
}

TEST_CASE("a planet file refuses what it doesn't know or can't be", "[globe][planet]") {
    for (const char* text : {
             "",
             "[planet]\ngravty = 3\n",
             "[planet]\ngravity = \"high\"\n",
             "[planet]\ngravity = -1\n",
             "[planet]\nseed = -1\n",
             "[planet]\nplates = 2.5\n",
             "[planet]\nstar_flux = 1000\n",
             "[planet]\nradius_km = 135\n",
             "[planet]\nday_hours = 4\n",
             "[planet]\nformed_detail = 0\n",
             "[planet]\nwater_depth = 2000\n",
             "[planet.ore_abundance]\nmithril = 1\n",
             "[planet]\n\n[globe]\nradius = 1000.0\n",
             "[planet\n",
         }) {
        CAPTURE(text);
        CHECK_FALSE(hog::globe::parse_planet(text).has_value());
    }
    CHECK(hog::globe::parse_planet("[planet\n").error().code == hog::ErrorCode::Parse);
    CHECK_FALSE(hog::globe::load_planet("no/such/planet.toml").has_value());
}

TEST_CASE("a world's id tells its knobs apart", "[globe][planet]") {
    const Planet earth;
    Planet other = earth;
    other.body.seed = 2;
    Planet larger = earth;
    larger.body.radius = 120'000.0;
    Planet finer = earth;
    finer.formed_detail = 6.0;
    Planet softer = earth;
    softer.body.erodibility *= 2.0;
    Planet moonless = earth;
    moonless.system.moons.clear();
    CHECK(hog::globe::world_id(earth) == hog::globe::world_id(Planet{}));
    for (const Planet& changed : {other, larger, finer, softer, moonless, frozen()}) {
        CHECK(hog::globe::world_id(earth) != hog::globe::world_id(changed));
    }
}

TEST_CASE("a small world's ground stays inside its shell, under the sky kept over its heights",
          "[globe][planet]") {
    Planet small;
    small.body.radius = 15'000.0;
    small.floor_depth = 500.0;
    // Lower than its mountains stand: the sky is kept over them, not cut through them.
    small.sky_height = 300.0;
    const hog::globe::Generator generator{small};
    const double ceiling = generator.layers().ceiling() - small.body.radius;
    for (std::uint64_t k = 0; k < 98'304; k += 997) {
        const auto ground = hog::globe::ground_below(
            generator, hog::globe::center(hog::globe::cell_at_index(7, k)), 20.0);
        REQUIRE(ground.has_value());
        const double height = ground.value_or(0.0) - small.body.radius;
        CHECK(height + small.sky_height < ceiling + generator.layers().thickness);
        CHECK(height > -small.floor_depth - 1'000.0);
    }
}

TEST_CASE("a planet sent as bytes is the same planet, and a cut or odd one is refused",
          "[globe][planet]") {
    Planet odd;
    odd.body.seed = 0xFEED'FACE'1234ULL;
    odd.body.radius = 12'345.678;
    odd.body.plates = 3;
    odd.body.ore_abundance.oil = 0.25;
    odd.heat_flow = 0.02;
    odd.formed_detail = 7.0;
    odd.system.rings = hog::planet::Rings{};
    for (const Planet& planet : {Planet{}, frozen(), odd}) {
        const auto bytes = hog::globe::encode_planet(planet);
        const auto read = hog::globe::decode_planet(bytes);
        REQUIRE(read);
        CHECK(hog::globe::world_id(*read) == hog::globe::world_id(planet));
        CHECK(read->body.seed == planet.body.seed);
        CHECK(read->system == planet.system);
        CHECK(read->body.ore_abundance.oil == planet.body.ore_abundance.oil);
    }
    const auto bytes = hog::globe::encode_planet(odd);
    for (std::size_t length = 0; length < bytes.size(); length += 7) {
        CHECK_FALSE(hog::globe::decode_planet(std::span{bytes}.first(length)));
    }
    CHECK_FALSE(hog::globe::decode_planet(std::span{bytes}.first(bytes.size() - 1)));
    auto longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(hog::globe::decode_planet(longer));
}
