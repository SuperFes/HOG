#include <hog/client/flight.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using Catch::Approx;
using hog::DVec3;
using hog::client::Flight;
using hog::client::flight_camera;
using hog::client::fly;
using hog::client::start_flight;

namespace {

constexpr double radius = 100'000.0;
constexpr double margin = 1e-9;

DVec3 up_at(const Flight& flight) {
    return glm::normalize(flight.eye.metres());
}
double height_of(const Flight& flight) {
    return glm::length(flight.eye.metres()) - radius;
}

// Along the ground at right angles to up, as a heading must be.
void check_level(const Flight& flight) {
    CHECK(glm::length(flight.heading) == Approx(1.0));
    CHECK(glm::dot(flight.heading, up_at(flight)) == Approx(0.0).margin(margin));
}

} // namespace

TEST_CASE("a flight starts over its place, facing along the ground towards a point",
          "[client][flight]") {
    const Flight flight = start_flight({1.0, 0.0, 0.0}, radius, 500.0, {1.0, 1.0, 0.0});
    CHECK(flight.eye.metres().x == Approx(radius + 500.0));
    CHECK(flight.heading.y == Approx(1.0));
    CHECK(flight.pitch == 0.0);
    check_level(flight);

    // Straight down or up, it faces north; at a pole, which has no north, along +x.
    CHECK(start_flight({1.0, 0.0, 0.0}, radius, 0.0, {1.0, 0.0, 0.0}).heading.z == Approx(1.0));
    CHECK(start_flight({0.0, 0.0, 1.0}, radius, 0.0, {0.0, 0.0, 2.0}).heading.x == Approx(1.0));
}

TEST_CASE("flying forwards goes the way it faces, faster the higher it is", "[client][flight]") {
    const hog::client::FlySettings settings{.speed = 0.5, .slowest = 3.0, .lowest = 2.0};
    const Flight start = start_flight({1.0, 0.0, 0.0}, radius, 1'002.0, {0.0, 1.0, 0.0});
    const Flight high =
        fly(start, {.move = {0.0, 0.0, 1.0}, .seconds = 0.01}, radius, 0.0, settings);
    // Half the height above what the eye keeps over the ground, a second.
    CHECK(high.eye.metres().y == Approx(5.0));
    check_level(high);

    const Flight low = start_flight({1.0, 0.0, 0.0}, radius, 3.0, {0.0, 1.0, 0.0});
    CHECK(
        fly(low, {.move = {0.0, 0.0, 1.0}, .seconds = 1.0}, radius, 0.0, settings).eye.metres().y ==
        Approx(3.0));
    // Right and up go right and up.
    const Flight moved =
        fly(start, {.move = {1.0, 1.0, 0.0}, .seconds = 0.01}, radius, 0.0, settings);
    CHECK(moved.eye.metres().z == Approx(-5.0));
    CHECK(height_of(moved) == Approx(1'007.0));
}

TEST_CASE("looking around turns and pitches the view, short of straight up or down",
          "[client][flight]") {
    const hog::client::FlySettings settings{.look = {.per_pixel = 0.01, .steepest = 1.0}};
    const Flight start = start_flight({1.0, 0.0, 0.0}, radius, 100.0, {0.0, 1.0, 0.0});
    const double quarter = std::numbers::pi / 2.0 / settings.look.per_pixel;
    // Facing +y with +x up, right is -z.
    const Flight turned = fly(start, {.look = {quarter, 0.0}}, radius, 0.0, settings);
    CHECK(turned.heading.z == Approx(-1.0));
    check_level(turned);

    const Flight down = fly(start, {.look = {0.0, 50.0}}, radius, 0.0, settings);
    CHECK(down.pitch == Approx(-0.5));
    CHECK(flight_camera(down, 1.0).forward.x == Approx(-std::sin(0.5)));
    CHECK(fly(start, {.look = {0.0, -500.0}}, radius, 0.0, settings).pitch == Approx(1.0));
    CHECK(fly(start, {.look = {0.0, 500.0}}, radius, 0.0, settings).pitch == Approx(-1.0));
}

TEST_CASE("the eye keeps above the ground", "[client][flight]") {
    const hog::client::FlySettings settings{.lowest = 2.0};
    const Flight start = start_flight({0.0, 1.0, 0.0}, radius, 300.0, {0.0, 0.0, 1.0});
    const Flight dived =
        fly(start, {.move = {0.0, -1.0, 0.0}, .seconds = 100.0}, radius, 250.0, settings);
    CHECK(height_of(dived) == Approx(252.0));
    CHECK(up_at(dived).y == Approx(1.0));
}

TEST_CASE("flying round the planet keeps the horizon level, over the poles too",
          "[client][flight]") {
    // From the equator due north, over the pole and down the far side.
    Flight flight = start_flight({1.0, 0.0, 0.0}, radius, 2'000.0, hog::planet::north_pole);
    flight.pitch = 0.3;
    double highest = 0.0;
    for (int step = 0; step < 400; ++step) {
        const DVec3 before = flight.heading;
        flight = fly(flight, {.move = {0.0, 0.0, 1.0}, .seconds = 1.0}, radius, 0.0,
                     {.speed = 0.0, .slowest = 1'000.0});
        flight.eye = hog::WorldPosition{up_at(flight) * (radius + 2'000.0)};
        highest = std::max(highest, up_at(flight).z);
        check_level(flight);
        // Never turning on its own.
        CHECK(glm::dot(flight.heading, before) > 0.99);
        const hog::render::Camera camera = flight_camera(flight, 1.0);
        CHECK(glm::length(camera.forward) == Approx(1.0));
        CHECK(glm::dot(camera.forward, camera.up) == Approx(0.0).margin(margin));
        CHECK(glm::dot(camera.up, up_at(flight)) == Approx(std::cos(0.3)));
    }
    CHECK(highest > 0.999);
    CHECK(up_at(flight).x < -0.5);
    CHECK(std::abs(up_at(flight).y) < 1e-6);
}

TEST_CASE("a viewpoint sees what the camera does, through the viewport", "[client][flight]") {
    const hog::render::Camera camera =
        flight_camera(start_flight({0.0, 0.0, -1.0}, radius, 10.0, {1.0, 0.0, 0.0}), 0.8);
    const hog::terrain::Viewpoint view =
        hog::client::viewpoint(camera, {.width = 1'600, .height = 800});
    CHECK(view.eye == camera.eye);
    CHECK(view.forward == camera.forward);
    CHECK(view.up == camera.up);
    CHECK(view.vertical_fov == 0.8);
    CHECK(view.aspect == 2.0);
    CHECK(view.viewport_height == 800.0);
}

TEST_CASE("a companion flies along ahead of the eye, landing where the eye is low",
          "[client][flight]") {
    const Flight flight = start_flight({1.0, 0.0, 0.0}, radius, 500.0, {0.0, 1.0, 0.0});
    const hog::WorldPosition high = hog::client::companion_feet(flight, 8.0, 2.0, radius, 100.0);
    CHECK(glm::length(high.metres()) == Approx(radius + 498.0));
    CHECK(glm::normalize(high.metres()).y == Approx(8.0 / (radius + 500.0)));
    CHECK(glm::dot(hog::client::companion_direction(flight, 8.0), glm::normalize(high.metres())) ==
          Approx(1.0));

    const hog::WorldPosition landed = hog::client::companion_feet(flight, 8.0, 2.0, radius, 499.0);
    CHECK(glm::length(landed.metres()) == Approx(radius + 499.0));
}

TEST_CASE("carried along with someone, the eye keeps facing the same way along the ground, round "
          "the planet and over the poles",
          "[client][flight]") {
    Flight flight = start_flight({1.0, 0.0, 0.0}, radius, 1.6, {0.0, 0.0, 1.0});
    flight.pitch = -0.3;
    const int steps = 1'000;
    for (int step = 1; step <= steps; ++step) {
        const double turned = 2.0 * std::numbers::pi * step / steps;
        flight = hog::client::carried_to(
            flight,
            hog::WorldPosition{DVec3{std::cos(turned), 0.0, std::sin(turned)} * (radius + 1.6)});
        check_level(flight);
    }
    CHECK(flight.eye.metres().x == Approx(radius + 1.6));
    CHECK(flight.heading.z == Approx(1.0));
    CHECK(flight.pitch == -0.3);
}

TEST_CASE("looking turns and pitches the eye where it is", "[client][flight]") {
    const Flight flight = start_flight({1.0, 0.0, 0.0}, radius, 2.0, {0.0, 1.0, 0.0});
    const hog::client::LookSettings settings{.per_pixel = 0.01, .steepest = 1.0};
    // Turning right from facing +y with +x up faces towards -z.
    const Flight turned = hog::client::looked(flight, {50.0, 0.0}, settings);
    CHECK(turned.heading.z == Approx(-std::sin(0.5)));
    CHECK(turned.eye == flight.eye);
    check_level(turned);
    CHECK(hog::client::looked(flight, {0.0, -50.0}, settings).pitch == Approx(0.5));
    CHECK(hog::client::looked(flight, {0.0, 500.0}, settings).pitch == Approx(-1.0));
}

TEST_CASE("from the third person, the eye looks at its mark from behind", "[client][flight]") {
    Flight flight = start_flight({1.0, 0.0, 0.0}, radius, 1.6, {0.0, 1.0, 0.0});
    const hog::render::Camera level = hog::client::third_person(flight, 4.0, 1.0);
    CHECK(level.eye.metres().x == Approx(radius + 1.6));
    CHECK(level.eye.metres().y == Approx(-4.0));
    CHECK(level.forward.y == Approx(1.0));
    CHECK(level.up.x == Approx(1.0));
    CHECK(level.vertical_fov == 1.0);

    // Looking down on it from above.
    flight.pitch = -std::numbers::pi / 6.0;
    const hog::render::Camera above = hog::client::third_person(flight, 4.0, 1.0);
    CHECK(above.eye.metres().x == Approx(radius + 1.6 + 2.0));
    CHECK(glm::length(above.eye - flight.eye) == Approx(4.0));
    CHECK(glm::dot(glm::normalize(flight.eye - above.eye), above.forward) == Approx(1.0));
}
