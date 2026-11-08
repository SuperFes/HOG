#include <hog/formation/plates.hpp>
#include <hog/formation/sediment.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>

using Catch::Matchers::WithinRel;
using hog::formation::Body;
using hog::formation::carrying_capacity;
using hog::formation::Climate;
using hog::formation::climate_on;
using hog::formation::Crust;
using hog::formation::crust_on;
using hog::formation::Lakes;
using hog::formation::lakes_on;
using hog::formation::Plates;
using hog::formation::Rivers;
using hog::formation::rivers_on;
using hog::formation::Sediment;
using hog::formation::sediment_on;
using hog::formation::Sphere;

TEST_CASE("rivers carry more with more water and on steeper ground", "[formation][sediment]") {
    const Body body;
    CHECK(carrying_capacity(2.0e9, 0.01, body) > carrying_capacity(1.0e9, 0.01, body));
    CHECK(carrying_capacity(1.0e9, 0.02, body) > carrying_capacity(1.0e9, 0.01, body));
    CHECK(carrying_capacity(1.0e9, 0.0, body) == 0.0);
}

namespace {

struct Made {
    Body body;
    Sphere layout{5, body.seed};
    Sphere sphere{6, body.seed + 1};
    Plates plates{body, layout};
    Crust crust = crust_on(sphere, plates, body);
    Climate climate = climate_on(sphere, crust.base, crust.sea, body);
    Rivers rivers = rivers_on(sphere, crust, climate, body);
    Lakes lakes = lakes_on(sphere, rivers, climate, body);
    Sediment sediment = sediment_on(sphere, rivers, lakes, body);
};

} // namespace

TEST_CASE("every bit of rock worn off settles somewhere", "[formation][sediment]") {
    const Made made;
    constexpr double earth_radius = 6'371'000.0;
    double worn = 0.0;
    double settled = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        const bool land = made.rivers.receiver[point] != point;
        if (land && made.lakes.basin[point] == Lakes::none) {
            worn += made.rivers.wear[point] * made.sphere.area(point) * earth_radius * earth_radius;
        }
        settled += made.sediment.settling[point];
        CHECK(made.sediment.settling[point] >= 0.0);
        CHECK(made.sediment.alluvium[point] >= 0.0);
        CHECK(made.sediment.delta[point] >= 0.0);
        CHECK(made.sediment.lake_depth[point] <= made.lakes.depth[point]);
    }
    CHECK_THAT(settled, WithinRel(worn, 1e-9));
}

TEST_CASE("hollows nothing leaves keep all that runs into them", "[formation][sediment]") {
    const Made made;
    for (const auto& basin : made.lakes.basins) {
        if (basin.lake == Lakes::none || !made.lakes.lakes[basin.lake].open) {
            CHECK(made.sediment.flux[basin.outlet] == 0.0);
        }
    }
}

TEST_CASE("lowlands gather more than mountains", "[formation][sediment]") {
    const Made made;
    double low = 0.0;
    double high = 0.0;
    int lows = 0;
    int highs = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.rivers.receiver[point] == point) {
            continue;
        }
        if (made.rivers.height[point] < 200.0) {
            low += made.sediment.alluvium[point];
            ++lows;
        } else if (made.rivers.height[point] > 2'000.0) {
            high += made.sediment.alluvium[point];
            ++highs;
        }
    }
    REQUIRE(lows > 0);
    REQUIRE(highs > 0);
    CHECK(low / lows > high / highs);
}
