#include <hog/client/flight.hpp>
#include <hog/client/sonar.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using Catch::Approx;
using hog::DVec3;
using hog::client::blips;
using hog::globe::Pocket;

namespace {

constexpr double radius = 100'000.0;
constexpr hog::DVec2 size{1600.0, 900.0};

// The eye 10 m under the planet's top, looking along +x on the ground there.
hog::client::Flight flight() {
    return hog::client::start_flight({0.0, 0.0, 1.0}, radius, 0.0, {1.0, 0.0, 0.0});
}

hog::terrain::Bend flat() {
    return {.up = {0.0, 0.0, 1.0}, .eye_height = 0.0, .radius = radius, .faked = radius};
}

Pocket pocket_at(const hog::client::Flight& flight, DVec3 along_ground, double rise) {
    const DVec3 eye = flight.eye.metres();
    const DVec3 up = glm::normalize(eye);
    return {.at = eye + along_ground + (up * rise), .across = 10.0, .height = 4.0};
}

} // namespace

TEST_CASE("a pocket ahead is marked over where it is, with how far and how deep",
          "[client][sonar]") {
    const hog::client::Flight eye = flight();
    const hog::render::Camera camera = hog::client::flight_camera(eye, 1.0);
    const std::vector<Pocket> pockets{pocket_at(eye, eye.heading * 80.0, -20.0)};
    const auto found = blips(pockets, camera, flat(), size, 40.0);
    REQUIRE(found.size() == 1);
    CHECK(found[0].on_screen);
    CHECK(found[0].at.x == Approx(size.x / 2.0).margin(1.0));
    // Below the middle, as it's below the eye.
    CHECK(found[0].at.y > size.y / 2.0);
    CHECK(found[0].distance == Approx(std::hypot(80.0, 20.0)));
    CHECK(found[0].text.find("down") != std::string::npos);
}

TEST_CASE("a pocket behind or to the side is held to the picture's edge, pointing the way",
          "[client][sonar]") {
    const hog::client::Flight eye = flight();
    const hog::render::Camera camera = hog::client::flight_camera(eye, 1.0);
    const DVec3 right = glm::cross(eye.heading, glm::normalize(eye.eye.metres()));
    const std::vector<Pocket> pockets{pocket_at(eye, -eye.heading * 60.0, 0.0),
                                      pocket_at(eye, -right * 60.0, 0.0)};
    const auto found = blips(pockets, camera, flat(), size, 40.0);
    REQUIRE(found.size() == 2);
    for (const auto& blip : found) {
        CHECK_FALSE(blip.on_screen);
        CHECK(blip.at.x >= 0.0);
        CHECK(blip.at.x <= size.x);
        CHECK(blip.at.y >= 0.0);
        CHECK(blip.at.y <= size.y);
    }
    // To the left, the arrow points left, at the left edge.
    const auto left =
        std::ranges::find_if(found, [](const auto& blip) { return blip.text.starts_with("<-"); });
    REQUIRE(left != found.end());
    CHECK(left->at.x < 100.0);
}

TEST_CASE("only the nearest pockets are marked, the furthest drawn first", "[client][sonar]") {
    const hog::client::Flight eye = flight();
    const hog::render::Camera camera = hog::client::flight_camera(eye, 1.0);
    std::vector<Pocket> pockets;
    for (int k = 1; k <= 12; ++k) {
        pockets.push_back(pocket_at(eye, eye.heading * (10.0 * k), -5.0));
    }
    const auto found = blips(pockets, camera, flat(), size, 40.0);
    CHECK(found.size() == hog::client::ping_shown);
    CHECK(std::ranges::is_sorted(found, std::ranges::greater{}, &hog::client::Blip::distance));
}
