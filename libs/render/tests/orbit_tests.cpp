#include <hog/render/orbit.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>

using Catch::Matchers::WithinAbs;
using hog::DVec2;
using hog::DVec3;
using hog::render::AltitudeRange;
using hog::render::Orbit;

namespace {

constexpr double radius = 1'000.0;
constexpr double pi = std::numbers::pi;

} // namespace

TEST_CASE("an orbit looks down at the planet with north up", "[render][orbit]") {
    const Orbit orbit{.latitude = 0.5, .longitude = 2.0, .altitude = 250.0};
    const auto camera = hog::render::orbit_camera(orbit, radius, 1.0);
    const DVec3 eye = camera.eye.metres();
    CHECK_THAT(glm::length(eye), WithinAbs(1'250.0, 1e-9));
    CHECK_THAT(std::asin(eye.z / glm::length(eye)), WithinAbs(0.5, 1e-12));
    CHECK_THAT(std::atan2(eye.y, eye.x), WithinAbs(2.0, 1e-12));
    CHECK_THAT(glm::dot(camera.forward, -glm::normalize(eye)), WithinAbs(1.0, 1e-12));
    CHECK_THAT(glm::dot(camera.forward, camera.up), WithinAbs(0.0, 1e-12));
    CHECK(camera.up.z > 0.0);
    CHECK(camera.near_plane < orbit.altitude);
    CHECK(camera.vertical_fov == 1.0);
}

TEST_CASE("dragging moves the ground with the cursor", "[render][orbit]") {
    const Orbit start{.altitude = radius};
    const Orbit east = hog::render::dragged(start, {100.0, 0.0}, 0.001, radius);
    CHECK_THAT(east.longitude, WithinAbs(-0.1, 1e-12));
    CHECK(east.latitude == 0.0);
    const Orbit down = hog::render::dragged(start, {0.0, 100.0}, 0.001, radius);
    CHECK_THAT(down.latitude, WithinAbs(0.1, 1e-12));

    // Closer in, the same drag covers less ground.
    const Orbit low =
        hog::render::dragged({.altitude = radius / 10.0}, {100.0, 0.0}, 0.001, radius);
    CHECK_THAT(low.longitude, WithinAbs(-0.01, 1e-12));
}

TEST_CASE("dragging stops short of the poles and wraps around", "[render][orbit]") {
    const Orbit north = hog::render::dragged({.altitude = radius}, {0.0, 1e6}, 0.001, radius);
    CHECK(north.latitude < pi / 2.0);
    CHECK(north.latitude > 0.99 * pi / 2.0);
    const Orbit south = hog::render::dragged({.altitude = radius}, {0.0, -1e6}, 0.001, radius);
    CHECK(south.latitude == -north.latitude);

    const Orbit around =
        hog::render::dragged({.longitude = 3.0, .altitude = radius}, {-500.0, 0.0}, 0.001, radius);
    CHECK_THAT(around.longitude, WithinAbs(3.5 - (2.0 * pi), 1e-12));
    CHECK(std::isfinite(hog::render::dragged(north, {100.0, 0.0}, 0.001, radius).longitude));
}

TEST_CASE("zooming scales the altitude within its range", "[render][orbit]") {
    const AltitudeRange range{.lowest = 10.0, .highest = 5'000.0};
    const Orbit start{.altitude = 1'000.0};
    const Orbit in = hog::render::zoomed(start, 1.0, range);
    CHECK(in.altitude < start.altitude);
    CHECK_THAT(hog::render::zoomed(in, -1.0, range).altitude, WithinAbs(1'000.0, 1e-9));
    CHECK(hog::render::zoomed(start, 100.0, range).altitude == range.lowest);
    CHECK(hog::render::zoomed(start, -100.0, range).altitude == range.highest);
}
