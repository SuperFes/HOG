#include <hog/planet/system.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <span>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;
using hog::planet::astronomical_unit;
using hog::planet::parse_system;
using hog::planet::System;

namespace {

System parsed(const std::string& rest) {
    auto system = parse_system("[planet]\n" + rest);
    INFO((system ? std::string{} : system.error().message));
    REQUIRE(system.has_value());
    return system.value_or(System{});
}

} // namespace

TEST_CASE("a planet left to its defaults sits in Earth's system", "[planet][system]") {
    const System system = parsed("");

    CHECK(system == hog::planet::System{});
    REQUIRE(system.stars.size() == 1);
    CHECK(system.stars.front().luminosity == 1.0);
    CHECK(system.stars.front().orbit.distance == 0.0);
    CHECK_THAT(system.orbit.distance, WithinRel(astronomical_unit, 1e-5));
    CHECK_THAT(system.orbit.period, WithinRel(1.0, 1e-4));
    REQUIRE(system.moons.size() == 1);
    CHECK(system.moons.front().orbit.distance == 3.844e8);
    CHECK_THAT(system.moons.front().orbit.period, WithinRel(29.53, 1e-4));
    CHECK_FALSE(system.rings.has_value());
    CHECK(system.air.pressure == 101'325.0);

    // Mercury, Venus, Mars, Jupiter and Saturn, in order out from the Sun.
    REQUIRE(system.planets.size() == 5);
    for (std::size_t index = 1; index < system.planets.size(); ++index) {
        CHECK(system.planets[index].orbit.distance > system.planets[index - 1].orbit.distance);
    }
    CHECK_THAT(system.planets[1].orbit.distance, WithinRel(0.723 * astronomical_unit, 1e-3));
    CHECK_THAT(system.planets[3].radius, WithinRel(69'911'000.0, 1e-9));
}

TEST_CASE("a config puts two suns, two moons and rings round the planet", "[planet][system]") {
    const System system = parsed(R"(
        planets = []

        [planet.air]
        pressure_kpa = 80
        oxygen = 0.25
        carbon_dioxide_ppm = 400

        [planet.orbit]
        distance_au = 1.5
        eccentricity = 0.05
        period_years = 1

        [[planet.stars]]
        luminosity = 1.2
        orbit = { distance_au = 0.05, period_years = 0.02 }

        [[planet.stars]]
        luminosity = 0.3
        temperature = 3_500
        radius_km = 300_000
        orbit = { distance_au = 0.05, longitude = 180, period_years = 0.02 }

        [[planet.moons]]

        [[planet.moons]]
        radius_km = 500
        tide = 0.1
        orbit = { distance_km = 60_000, eccentricity = 0.01, period_days = 3.5 }

        [planet.rings]
        inner = 1.8
        outer = 3.0
    )");

    CHECK(system.planets.empty());
    CHECK_THAT(system.air.pressure, WithinRel(80'000.0, 1e-12));
    CHECK(system.air.oxygen == 0.25);
    CHECK_THAT(system.air.carbon_dioxide, WithinRel(4e-4, 1e-12));
    CHECK_THAT(system.orbit.distance, WithinRel(1.5 * astronomical_unit, 1e-12));
    CHECK(system.orbit.eccentricity == 0.05);
    // What isn't given stays Earth's.
    CHECK(system.orbit.periapsis == hog::planet::System{}.orbit.periapsis);

    REQUIRE(system.stars.size() == 2);
    CHECK(system.stars[0].luminosity == 1.2);
    CHECK(system.stars[0].temperature == hog::planet::Star{}.temperature);
    CHECK(system.stars[1].temperature == 3'500.0);
    CHECK_THAT(system.stars[1].radius, WithinRel(3.0e8, 1e-12));
    CHECK(system.stars[1].orbit.longitude == 180.0);
    CHECK(system.stars[1].orbit.period == 0.02);

    REQUIRE(system.moons.size() == 2);
    CHECK(system.moons[0] == hog::planet::Moon{});
    CHECK_THAT(system.moons[1].radius, WithinRel(500'000.0, 1e-12));
    CHECK_THAT(system.moons[1].orbit.distance, WithinRel(6.0e7, 1e-12));
    CHECK(system.moons[1].orbit.period == 3.5);
    CHECK(system.moons[1].tide == 0.1);

    REQUIRE(system.rings.has_value());
    CHECK(system.rings.value_or(hog::planet::Rings{}).inner == 1.8);
    CHECK(system.rings.value_or(hog::planet::Rings{}).outer == 3.0);
    CHECK(system.rings.value_or(hog::planet::Rings{}).opacity == hog::planet::Rings{}.opacity);
}

TEST_CASE("a planet may have no moons", "[planet][system]") {
    CHECK(parsed("moons = []\n").moons.empty());
}

TEST_CASE("a config sets how the orbit and axis wander", "[planet][system]") {
    const hog::planet::Cycles cycles = parsed(R"(
        [planet.cycles]
        tilt = [{ size = 5, period_years = 100_000, phase = 90 }]
        eccentricity = []
        precession_years = -30_000
    )")
                                           .cycles;
    REQUIRE(cycles.tilt.size() == 1);
    CHECK(cycles.tilt.front() ==
          hog::planet::Cycle{.size = 5.0, .period = 100'000.0, .phase = 90.0});
    CHECK(cycles.eccentricity.empty());
    CHECK(cycles.precession == -30'000.0);
}

TEST_CASE("bad system settings are rejected with a reason", "[planet][system]") {
    const auto [rest, reason] = GENERATE(table<std::string, std::string>({
        {"stars = []\n", "at least one star"},
        {"moons = 3\n", "list of tables"},
        {"moons = [1]\n", "planet.moons[0] must be a table"},
        {"[[planet.stars]]\ncolour = 1\n", "unknown key planet.stars[0].colour"},
        {"[[planet.stars]]\nluminosity = 0\n", "planet.stars[0].luminosity"},
        {"[planet.orbit]\neccentricity = 1\n", "planet.orbit.eccentricity"},
        {"[planet.orbit]\ndistance_au = 0\n", "planet.orbit.distance_au"},
        {"[planet.orbit]\ndistance_km = 1\n", "unknown key planet.orbit.distance_km"},
        {"[[planet.moons]]\norbit = { distance_km = 0 }\n", "planet.moons[0].orbit.distance_km"},
        {"[[planet.moons]]\norbit = { period_years = 1 }\n", "unknown key"},
        {"[[planet.planets]]\nalbedo = 0.5\n", "planet.planets[0] needs radius_km"},
        {"[planet.rings]\ninner = 2\nouter = 1.5\n", "beyond inner"},
        {"[planet.rings]\ninner = 0.5\n", "planet.rings.inner"},
        {"[planet.air]\noxygen = 0.9\ncarbon_dioxide_ppm = 200_000\n", "more than the air"},
        {"air = 1\n", "planet.air must be a table"},
        {"[planet.cycles]\ntilt = [{ size = 1 }]\n", "planet.cycles.tilt[0] needs period_years"},
        {"[planet.cycles]\ntilt = [{ size = 91, period_years = 1000 }]\n",
         "planet.cycles.tilt[0].size"},
        {"[planet.cycles]\neccentricity = [{ size = 0.6, period_years = 1000 }]\n",
         "planet.cycles.eccentricity[0].size"},
        {"[planet.cycles]\nwobble = 1\n", "unknown key planet.cycles.wobble"},
    }));
    CAPTURE(rest);
    const auto system = parse_system("[planet]\n" + rest);
    REQUIRE_FALSE(system.has_value());
    CHECK(system.error().code == hog::ErrorCode::InvalidArgument);
    CHECK_THAT(system.error().message, ContainsSubstring(reason));
}

TEST_CASE("a system goes to bytes and back the same, to the bit", "[planet][system]") {
    System system = parsed(R"(
        [planet.rings]
        inner = 1.8
        outer = 3.0

        [[planet.moons]]

        [[planet.moons]]
        radius_km = 500
    )");
    system.air.oxygen = 0.3;
    hog::ByteWriter out;
    hog::planet::write_system(out, system);
    const std::vector<std::byte> bytes = std::move(out).take();
    hog::ByteReader in{bytes};
    System read;
    REQUIRE(hog::planet::read_system(in, read));
    CHECK(in.remaining() == 0);
    CHECK(read == system);
    CHECK(hog::planet::system_id(read) == hog::planet::system_id(system));

    hog::ByteReader cut{std::span{bytes}.first(bytes.size() - 1)};
    System partial;
    CHECK_FALSE(hog::planet::read_system(cut, partial));
}

TEST_CASE("the system's keys are told from the rest of [planet]", "[planet][system]") {
    CHECK(hog::planet::system_key("moons"));
    CHECK(hog::planet::system_key("air"));
    CHECK_FALSE(hog::planet::system_key("radius"));
    CHECK_FALSE(parse_system("[globe]\n"));
}
