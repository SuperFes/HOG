#include <hog/planet/planet_config.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;
using hog::ErrorCode;
using hog::planet::parse_planet_config;

TEST_CASE("population sizing matches the table in the plan", "[planet][config]") {
    const auto radius = [](double population) {
        return hog::planet::radius_for_population({.population = population}, 0.7);
    };
    CHECK_THAT(radius(500'000), WithinRel(115'000.0, 0.01));
    CHECK_THAT(radius(2'000'000), WithinRel(230'000.0, 0.01));
    CHECK_THAT(radius(10'000'000), WithinRel(515'000.0, 0.01));
}

TEST_CASE("a population config computes the radius", "[planet][config]") {
    const auto config = parse_planet_config(R"(
        [planet]
        seed = 42
        ocean_fraction = 0.5
        population = 1_000
        settled_density = 10
        wilderness_factor = 2.0
    )");
    REQUIRE(config.has_value());
    CHECK(config->seed == 42);
    CHECK(config->ocean_fraction == 0.5);
    // 1000 / 10 * 2 = 200 km² of land, 400 km² in all.
    CHECK_THAT(config->radius,
               WithinRel(std::sqrt(400.0 / (4.0 * 3.141592653589793)) * 1'000.0, 1e-12));
}

TEST_CASE("a radius config skips sizing and defaults the ocean", "[planet][config]") {
    const auto config = parse_planet_config("[planet]\nseed = 0\nradius_km = 6_371.0\n");
    REQUIRE(config.has_value());
    CHECK(config->radius == 6'371'000.0);
    CHECK(config->ocean_fraction == 0.7);
    CHECK(config->day_length == std::chrono::hours{4});
    CHECK(config->axial_tilt == 23.44);
}

TEST_CASE("a config sets the day and the tilt", "[planet][config]") {
    const auto config =
        parse_planet_config("[planet]\nseed = 0\nradius_km = 1\nday_hours = 6.5\naxial_tilt = 0\n");
    REQUIRE(config.has_value());
    CHECK(config->day_length == std::chrono::minutes{390});
    CHECK(config->axial_tilt == 0.0);
}

TEST_CASE("the shipped development config is the 500k planet", "[planet][config]") {
    const auto config = hog::planet::load_planet_config(HOG_SOURCE_DIR "/config/planet.toml");
    REQUIRE(config.has_value());
    CHECK_THAT(config->radius, WithinRel(115'000.0, 0.01));
}

TEST_CASE("bad configs are rejected with a reason", "[planet][config]") {
    struct Case {
        const char* toml;
        ErrorCode code;
        const char* reason;
    };
    const auto bad = GENERATE(
        Case{"[planet\nseed = 1", ErrorCode::Parse, "line 1"},
        Case{"seed = 1", ErrorCode::InvalidArgument, "[planet]"},
        Case{"[planet]\nradius_km = 5", ErrorCode::InvalidArgument, "seed"},
        Case{"[planet]\nseed = -1\nradius_km = 5", ErrorCode::InvalidArgument, "seed"},
        Case{"[planet]\nseed = 1.5\nradius_km = 5", ErrorCode::InvalidArgument, "seed"},
        Case{"[planet]\nseed = 1", ErrorCode::InvalidArgument, "exactly one"},
        Case{"[planet]\nseed = 1\npopulation = 5\nradius_km = 5", ErrorCode::InvalidArgument,
             "exactly one"},
        Case{"[planet]\nseed = 1\nradius_km = 5\nsettled_density = 3", ErrorCode::InvalidArgument,
             "only apply"},
        Case{"[planet]\nseed = 1\nradius_km = -5", ErrorCode::InvalidArgument, "radius_km"},
        Case{"[planet]\nseed = 1\nradius_km = inf", ErrorCode::InvalidArgument, "radius_km"},
        Case{"[planet]\nseed = 1\npopulation = 0", ErrorCode::InvalidArgument, "positive"},
        Case{"[planet]\nseed = 1\npopulation = nan", ErrorCode::InvalidArgument, "positive"},
        Case{"[planet]\nseed = 1\nradius_km = 5\nocean_fraction = 1.0", ErrorCode::InvalidArgument,
             "ocean_fraction"},
        Case{"[planet]\nseed = 1\nradius_km = 5\nocean_fraction = 'wet'",
             ErrorCode::InvalidArgument, "number"},
        Case{"[planet]\nseed = 1\nradius_km = 5\nradius = 5", ErrorCode::InvalidArgument,
             "planet.radius"},
        Case{"[planet]\nseed = 1\nradius_km = 5\nday_hours = 0", ErrorCode::InvalidArgument,
             "day_hours"},
        Case{"[planet]\nseed = 1\nradius_km = 5\nday_hours = nan", ErrorCode::InvalidArgument,
             "day_hours"},
        Case{"[planet]\nseed = 1\nradius_km = 5\naxial_tilt = 91", ErrorCode::InvalidArgument,
             "axial_tilt"},
        Case{"[planet]\nseed = 1\nradius_km = 5\naxial_tilt = -1", ErrorCode::InvalidArgument,
             "axial_tilt"});

    const auto config = parse_planet_config(bad.toml);
    INFO(bad.toml);
    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().code == bad.code);
    CHECK_THAT(config.error().message, ContainsSubstring(bad.reason));
}

TEST_CASE("missing config files report not found with the path", "[planet][config]") {
    const auto config = hog::planet::load_planet_config("no/such/planet.toml");
    REQUIRE_FALSE(config.has_value());
    CHECK(config.error().code == ErrorCode::NotFound);
    CHECK_THAT(config.error().message, ContainsSubstring("no/such/planet.toml"));
}

TEST_CASE("population sizing checks its inputs", "[planet][config]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    CHECK_THROWS_AS(hog::planet::radius_for_population({.population = 0.0}, 0.7), AssertionFailed);
    CHECK_THROWS_AS(hog::planet::radius_for_population({.population = 1.0}, 1.0), AssertionFailed);
}

TEST_CASE("a world's id follows what shapes its ground", "[planet][config]") {
    const hog::planet::PlanetConfig base{.seed = 1, .ocean_fraction = 0.7, .radius = 115'000.0};
    const std::uint64_t id = hog::planet::world_id(base);
    // Saved worlds are keyed by this, so it must not change between builds.
    CHECK(id == 0xB87E7E64C83CED74ULL);

    auto other = base;
    other.seed = 2;
    CHECK(hog::planet::world_id(other) != id);
    other = base;
    other.radius = 115'001.0;
    CHECK(hog::planet::world_id(other) != id);
    other = base;
    other.ocean_fraction = 0.6;
    CHECK(hog::planet::world_id(other) != id);

    other = base;
    other.day_length = std::chrono::hours{24};
    other.axial_tilt = 0.0;
    CHECK(hog::planet::world_id(other) == id);
}
