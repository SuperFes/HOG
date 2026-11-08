#include <hog/formation/plates.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::formation::Body;
using hog::formation::Crust;
using hog::formation::crust_on;
using hog::formation::Meeting;
using hog::formation::Plates;
using hog::formation::Sphere;

TEST_CASE("every point of the layout lies on a plate", "[formation][plates]") {
    const Body body;
    const Sphere layout{5, body.seed};
    const Plates plates{body, layout};
    CHECK(plates.count() == body.plates);
    for (std::uint32_t point = 0; point < layout.size(); ++point) {
        CHECK(plates.plate(point) < plates.count());
    }
}

TEST_CASE("light crust covers about the share asked for", "[formation][plates]") {
    Body body;
    body.land_share = GENERATE(0.1, 0.3, 0.6);
    const Sphere layout{5, body.seed};
    const Plates plates{body, layout};
    double area = 0.0;
    for (std::uint32_t point = 0; point < layout.size(); ++point) {
        area += plates.light(point) ? layout.area(point) : 0.0;
    }
    CHECK_THAT(area / (4.0 * std::numbers::pi), WithinAbs(body.land_share, 0.01));
}

TEST_CASE("plates move along the surface, and pairs agree on their boundary",
          "[formation][plates]") {
    const Body body;
    const Sphere layout{4, body.seed};
    const Plates plates{body, layout};
    for (int one = 0; one < plates.count(); ++one) {
        const auto a = static_cast<std::uint16_t>(one);
        CHECK_THAT(glm::dot(plates.velocity(a, layout.point(a)), layout.point(a)),
                   WithinAbs(0.0, 1e-15));
        for (int other = 0; other < plates.count(); ++other) {
            const auto b = static_cast<std::uint16_t>(other);
            CHECK(plates.age(a, b) == plates.age(b, a));
            if (a != b) {
                CHECK(plates.rides_over(a, b) != plates.rides_over(b, a));
            }
        }
    }
}

namespace {

struct Made {
    Body body;
    Sphere layout{5, body.seed};
    Sphere sphere{6, body.seed + 1};
    Plates plates{body, layout};
    Crust crust = crust_on(sphere, plates, body);
};

} // namespace

TEST_CASE("crust stands high where light and deep where sea floor is old", "[formation][plates]") {
    const Made made;
    const Crust& crust = made.crust;
    double land = 0.0;
    double high = 0.0;
    double low = 0.0;
    int highs = 0;
    int lows = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        CHECK(crust.light[point] >= 0.0);
        CHECK(crust.light[point] <= 1.0);
        CHECK(crust.uplift[point] >= 0.0);
        if (crust.light[point] >= 0.5) {
            land += made.sphere.area(point);
            high += crust.base[point];
            ++highs;
        } else {
            low += crust.base[point];
            ++lows;
        }
    }
    CHECK_THAT(land / (4.0 * std::numbers::pi), WithinAbs(made.body.land_share, 0.05));
    REQUIRE(highs > 0);
    REQUIRE(lows > 0);
    CHECK(high / highs > 0.0);
    CHECK(low / lows < made.body.ridge_depth);
}

TEST_CASE("boundaries of each kind form, and belts stand high", "[formation][plates]") {
    const Made made;
    const Crust& crust = made.crust;
    std::vector<int> kinds(8, 0);
    double collision_height = 0.0;
    int collisions = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        ++kinds[static_cast<std::size_t>(crust.meeting[point])];
        if (crust.meeting[point] == Meeting::Collision && crust.boundary[point] < 0.01) {
            collision_height += crust.base[point];
            ++collisions;
        }
    }
    CHECK(kinds[static_cast<std::size_t>(Meeting::Ridge)] > 0);
    CHECK(kinds[static_cast<std::size_t>(Meeting::Transform)] > 0);
    CHECK(kinds[static_cast<std::size_t>(Meeting::Subduction)] +
              kinds[static_cast<std::size_t>(Meeting::IslandArc)] >
          0);
    if (collisions > 0) {
        CHECK(collision_height / collisions > made.body.continent_height);
    }
}

TEST_CASE("sea floor is young at the ridges", "[formation][plates]") {
    const Made made;
    const Crust& crust = made.crust;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (crust.meeting[point] == Meeting::Ridge && crust.boundary[point] < 0.005) {
            CHECK(crust.floor_age[point] < 20.0);
        }
    }
}

TEST_CASE("the same body makes the same crust", "[formation][plates]") {
    const Made one;
    const Made two;
    CHECK(one.crust.base == two.crust.base);
    CHECK(one.crust.uplift == two.crust.uplift);
}
