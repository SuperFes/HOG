#include <hog/sim/calendar.hpp>
#include <hog/viewer/daylight.hpp>
#include <hog/viewer/globe_view.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>

using namespace std::chrono;
using Catch::Matchers::WithinAbs;
using hog::viewer::GlobeCamera;

TEST_CASE("the globe's camera starts off the equator, looking at the planet", "[viewer][globe]") {
    const GlobeCamera camera;
    CHECK(camera.orbit().altitude == GlobeCamera::start_altitude);
    const hog::render::Camera seen = camera.camera();
    CHECK_THAT(glm::length(seen.eye.metres()), WithinAbs(1.0 + GlobeCamera::start_altitude, 1e-9));
    CHECK_THAT(glm::dot(seen.forward, glm::normalize(seen.eye.metres())), WithinAbs(-1.0, 1e-9));
}

TEST_CASE("dragging turns the globe and zooming stays in range", "[viewer][globe]") {
    GlobeCamera camera;
    // Dragging down brings the north into view; dragging right, the west.
    camera.drag({0.0, 100.0}, 500.0);
    CHECK(camera.orbit().latitude > 0.0);
    camera.drag({100.0, 0.0}, 500.0);
    CHECK(camera.orbit().longitude < 0.0);

    for (int step = 0; step < 100; ++step) {
        camera.zoom(1.0);
    }
    CHECK(camera.orbit().altitude == GlobeCamera::altitudes.lowest);
    for (int step = 0; step < 100; ++step) {
        camera.zoom(-1.0);
    }
    CHECK(camera.orbit().altitude == GlobeCamera::altitudes.highest);

    camera.reset();
    CHECK(camera.orbit() == GlobeCamera{}.orbit());
}

TEST_CASE("the globe is lit by the frame's sun when daylight is shown", "[viewer][globe]") {
    const GlobeCamera camera;
    CHECK(hog::viewer::globe_view(camera, nullptr, true).night_light == 1.0);

    hog::probe::Welcome welcome;
    welcome.day_length = hours{4};
    welcome.axial_tilt = 23.44;
    const hog::GameTime noon =
        hog::to_game_time(hog::WallClock::time_point{sys_days{2026y / March / 20} + 14h});
    const hog::viewer::Frame frame{welcome,
                                   hog::probe::Layers{.time = noon, .clock = {}, .layers = {}}, 0};
    const hog::render::GlobeView lit = hog::viewer::globe_view(camera, &frame, true);
    CHECK(lit.night_light == hog::viewer::night_light);
    CHECK(lit.twilight == hog::sim::twilight);
    const hog::DVec3 sun = hog::sim::Calendar{hours{4}, 23.44}.sun_direction(noon);
    CHECK_THAT(glm::distance(lit.sun, sun), WithinAbs(0.0, 1e-12));
    CHECK(hog::viewer::globe_view(camera, &frame, false).night_light == 1.0);

    // Without a usable calendar, evenly.
    const hog::viewer::Frame timeless{
        hog::probe::Welcome{}, hog::probe::Layers{.time = noon, .clock = {}, .layers = {}}, 0};
    CHECK(hog::viewer::globe_view(camera, &timeless, true).night_light == 1.0);
}
