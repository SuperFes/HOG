#include <hog/formation/plates.hpp>
#include <hog/formation/sediment.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

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

TEST_CASE("rock handed to the rivers settles somewhere too", "[formation][sediment]") {
    const Made made;
    constexpr double earth_radius = 6'371'000.0;
    const std::vector<double> brought(made.sphere.size(), 1.0e9);
    const Sediment sediment = sediment_on(made.sphere, made.rivers, made.lakes, brought, made.body);
    double arriving = 0.0;
    double settled = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        const bool land = made.rivers.receiver[point] != point;
        if (land && made.lakes.basin[point] == Lakes::none) {
            arriving +=
                made.rivers.wear[point] * made.sphere.area(point) * earth_radius * earth_radius;
        }
        arriving += brought[point] / made.body.sediment_age;
        settled += sediment.settling[point];
    }
    CHECK_THAT(settled, WithinRel(arriving, 1e-9));
    double more = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        more += sediment.delta[point] - made.sediment.delta[point];
    }
    CHECK(more > 0.0);
}

TEST_CASE("deltas build out over the sea floor no higher than the waterline",
          "[formation][sediment]") {
    const Made made;
    double laid = 0.0;
    int spread = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.rivers.receiver[point] != point) {
            continue;
        }
        CHECK(made.sediment.delta[point] <= -made.rivers.height[point] + 1e-6);
        laid += made.sediment.delta[point] * made.sphere.area(point);
        spread += made.sediment.delta[point] > 0.0 && made.sediment.settling[point] == 0.0 ? 1 : 0;
    }
    double settled = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.rivers.receiver[point] == point) {
            settled += made.sediment.settling[point] * made.body.sediment_age /
                       (6'371'000.0 * 6'371'000.0);
        }
    }
    CHECK_THAT(laid, WithinRel(settled, 1e-9));
    CHECK(spread > 0);
}

TEST_CASE("hollows keep what runs in up to their brim, and only then pass it on",
          "[formation][sediment]") {
    const Made made;
    constexpr double earth_radius = 6'371'000.0;
    std::vector<double> kept(made.lakes.basins.size(), 0.0);
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.lakes.basin[point] != Lakes::none) {
            kept[made.lakes.basin[point]] += made.sediment.alluvium[point] *
                                             made.sphere.area(point) * earth_radius * earth_radius;
        }
    }
    for (std::size_t basin = 0; basin < kept.size(); ++basin) {
        const auto& hollow = made.lakes.basins[basin];
        CHECK(kept[basin] <= (hollow.room * (1.0 + 1e-9)) + 1.0);
        if (made.sediment.flux[hollow.outlet] > 0.0) {
            CHECK_THAT(kept[basin], WithinRel(hollow.room, 1e-6));
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
