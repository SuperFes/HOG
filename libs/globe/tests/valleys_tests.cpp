#include "../src/valleys.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::globe::Valley;
using hog::globe::wall_at;

namespace {

constexpr double radius = 30'000.0;

// A kilometre-long valley running east along the equator, falling from 100 m to 80 m, its walls
// rising one in two.
Valley falling() {
    return {.from = {1.0, 0.0, 0.0},
            .to = glm::normalize(DVec3{1.0, 1'000.0 / radius, 0.0}),
            .floor_from = 100.0,
            .floor_to = 80.0,
            .wall = 0.5};
}

DVec3 at(double east, double north) {
    return glm::normalize(DVec3{1.0, east / radius, north / radius});
}

} // namespace

TEST_CASE("a valley's floor falls along it between its ends", "[globe][valleys]") {
    CHECK_THAT(wall_at(falling(), at(0.0, 0.0), radius).height, WithinAbs(100.0, 1e-6));
    CHECK_THAT(wall_at(falling(), at(500.0, 0.0), radius).height, WithinAbs(90.0, 0.01));
    CHECK_THAT(wall_at(falling(), at(1'000.0, 0.0), radius).height, WithinAbs(80.0, 1e-6));
}

TEST_CASE("a valley's walls rise from its floor as steeply as it's given", "[globe][valleys]") {
    CHECK_THAT(wall_at(falling(), at(500.0, 100.0), radius).height, WithinAbs(140.0, 0.05));
    CHECK_THAT(wall_at(falling(), at(500.0, -200.0), radius).height, WithinAbs(190.0, 0.05));
    // Past its end, up from the end's floor.
    CHECK_THAT(wall_at(falling(), at(1'300.0, 0.0), radius).height, WithinAbs(230.0, 0.5));
}

TEST_CASE("a valley's walls reach only so far, rising smoothly out of the way past half of it",
          "[globe][valleys]") {
    Valley near = falling();
    near.reach = 200.0;
    CHECK_THAT(wall_at(near, at(500.0, 50.0), radius).height, WithinAbs(115.0, 0.05));
    CHECK(wall_at(near, at(500.0, 200.0), radius).height > 1e4);
    // Without a step anywhere on the way.
    double last = wall_at(near, at(500.0, 0.0), radius).height;
    for (int metres = 1; metres <= 250; ++metres) {
        const double height = wall_at(near, at(500.0, metres), radius).height;
        CHECK(height >= last);
        CHECK(height - last < 2'000.0);
        last = height;
    }
}

TEST_CASE("a valley's walls rise away from it, and its floor falls along it", "[globe][valleys]") {
    const DVec3 north = wall_at(falling(), at(500.0, 100.0), radius).gradient;
    CHECK_THAT(north.z, WithinRel(0.5, 1e-3));
    CHECK_THAT(north.y, WithinAbs(-0.02, 1e-3));
    const DVec3 south = wall_at(falling(), at(500.0, -100.0), radius).gradient;
    CHECK_THAT(south.z, WithinRel(-0.5, 1e-3));
}
