#include <hog/formation/soil.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>

using Catch::Matchers::WithinRel;
using hog::formation::balanced_depth;
using hog::formation::Body;
using hog::formation::Climate;
using hog::formation::climate_on;
using hog::formation::crust_on;
using hog::formation::Plates;
using hog::formation::Rivers;
using hog::formation::rivers_on;
using hog::formation::Soil;
using hog::formation::soil_making;
using hog::formation::soil_on;
using hog::formation::Sphere;

TEST_CASE("rock weathers faster warm and wet, not at all frozen", "[formation][soil]") {
    const Body body;
    CHECK(soil_making(25.0, 2.0, body) > soil_making(10.0, 2.0, body));
    CHECK(soil_making(15.0, 2.0, body) > soil_making(15.0, 0.1, body));
    CHECK(soil_making(-40.0, 2.0, body) < 1e-3 * soil_making(15.0, 2.0, body));
    // Frost shatters rock best a few degrees under freezing.
    CHECK(soil_making(-5.0, 0.3, body) > soil_making(-15.0, 0.3, body));
    CHECK(soil_making(-5.0, 0.3, body) > soil_making(-5.0, 0.0, body));
    CHECK(soil_making(15.0, 0.0, body) == 0.0);
}

TEST_CASE("soil lies as deep as making balances wear", "[formation][soil]") {
    const Body body;
    const double making = 1.0e-4;
    const double depth = balanced_depth(making, 1.0e-5, body);
    // At that depth, rock under it turns to soil exactly as fast as the ground wears.
    CHECK_THAT(making * std::exp(-body.soil_falloff * depth), WithinRel(1.0e-5, 1e-9));
    CHECK(balanced_depth(making, 2.0e-4, body) == 0.0);
    CHECK(balanced_depth(making, 0.0, body) == balanced_depth(making, body.slowest_wear, body));
}

TEST_CASE("fast-rising mountains are bare and still lowlands soiled", "[formation][soil]") {
    const Body body;
    const Sphere layout{5, body.seed};
    const Sphere sphere{6, body.seed + 1};
    const Plates plates{body, layout};
    const auto crust = crust_on(sphere, plates, body);
    const Climate climate = climate_on(sphere, crust.base, crust.sea, body);
    const Rivers rivers = rivers_on(sphere, crust, climate, body);
    const Soil soil = soil_on(sphere, rivers, climate, body);
    int soiled = 0;
    int land = 0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        CHECK(soil.depth[point] >= 0.0);
        if (rivers.receiver[point] == point) {
            CHECK(soil.depth[point] == 0.0);
            continue;
        }
        ++land;
        soiled += soil.depth[point] > 0.0 ? 1 : 0;
        if (rivers.wear[point] > soil.making[point]) {
            CHECK(soil.depth[point] == 0.0);
        }
    }
    REQUIRE(land > 0);
    CHECK(soiled > land / 2);
}
