#include <hog/formation/lakes.hpp>

#include <hog/formation/plates.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

using Catch::Matchers::WithinRel;
using hog::formation::Body;
using hog::formation::Climate;
using hog::formation::climate_on;
using hog::formation::Crust;
using hog::formation::crust_on;
using hog::formation::fill_hollow;
using hog::formation::Filling;
using hog::formation::Lakes;
using hog::formation::lakes_on;
using hog::formation::open_water_loss;
using hog::formation::Plates;
using hog::formation::Rivers;
using hog::formation::rivers_on;
using hog::formation::Sphere;

TEST_CASE("open water loses more to the air warm than cold", "[formation][lakes]") {
    const Body body;
    CHECK(open_water_loss(25.0, body) > open_water_loss(15.0, body));
    CHECK_THAT(open_water_loss(15.0, body), WithinRel(body.lake_loss * 1.0, 0.2));
    CHECK(open_water_loss(-20.0, body) == 0.0);
}

TEST_CASE("a hollow fills until the air takes what flows in, or to its brim", "[formation][lakes]") {
    const std::array<double, 4> ground{10.0, 12.0, 15.0, 20.0};
    const std::array<double, 4> loss{1.0, 1.0, 1.0, 1.0};
    const Filling brim = fill_hollow(ground, loss, 10.0, 25.0);
    CHECK(brim.covered == 4);
    CHECK(brim.level == 25.0);
    const Filling part = fill_hollow(ground, loss, 2.5, 25.0);
    CHECK(part.covered == 2);
    CHECK(part.level == 15.0);
    const Filling dry = fill_hollow(ground, loss, 0.5, 25.0);
    CHECK(dry.covered == 0);
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
};

} // namespace

TEST_CASE("lakes stand in hollows, brim-full only where they can keep up", "[formation][lakes]") {
    const Made made;
    const Lakes& lakes = made.lakes;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        CHECK(lakes.depth[point] >= 0.0);
        CHECK((lakes.depth[point] > 0.0) <= (lakes.lake[point] != Lakes::none));
        CHECK(lakes.discharge[point] >= 0.0);
        CHECK(lakes.discharge[point] <= made.rivers.discharge[point] * (1.0 + 1e-12));
        if (lakes.lake[point] != Lakes::none) {
            CHECK(made.crust.sea[point] == 0);
            const auto& lake = lakes.lakes[lakes.lake[point]];
            CHECK(lake.level >= made.rivers.ground[point]);
        }
    }
    for (const auto& lake : lakes.lakes) {
        CHECK(lake.area > 0.0);
        if (!lake.open) {
            CHECK(lake.loss <= lake.inflow * (1.0 + 1e-12));
        }
    }
}

TEST_CASE("what reaches the sea is the rain on land less what lakes give the air",
          "[formation][lakes]") {
    const Made made;
    constexpr double earth_radius = 6'371'000.0;
    double fallen = 0.0;
    double out = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.crust.sea[point] != 0) {
            continue;
        }
        fallen += made.climate.rain[point] * made.sphere.area(point) * earth_radius * earth_radius;
        if (made.crust.sea[made.rivers.receiver[point]] != 0) {
            out += made.lakes.discharge[point];
        }
    }
    double kept = 0.0;
    for (const auto& lake : made.lakes.lakes) {
        kept += std::min(lake.inflow, lake.loss);
    }
    CHECK(kept <= made.lakes.to_air * (1.0 + 1e-12));
    CHECK(out <= fallen * (1.0 + 1e-9));
    // Water taken from a river already drained by one upstream can't go below none, so the sea
    // may get a little more than this, never less.
    CHECK(out >= (fallen - made.lakes.to_air) * (1.0 - 1e-9));
}
