#include <hog/formation_lab/pictures.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::formation::Sphere;
using hog::formation_lab::map_direction;
using hog::formation_lab::map_picture;
using hog::formation_lab::ramp;
using hog::formation_lab::Rgb;

TEST_CASE("a map is twice as wide as high, a pixel's colour each", "[formation][pictures]") {
    int calls = 0;
    const hog::Image image = map_picture(64, [&](const DVec3&) {
        ++calls;
        return hog::formation_lab::rgb(1, 2, 3);
    });
    CHECK(image.width == 64);
    CHECK(image.height == 32);
    CHECK(calls == 64 * 32);
    CHECK(image.pixels.size() == 64U * 32U * 3U);
    CHECK(image.pixels[0] == 1);
    CHECK(image.pixels[2] == 3);
}

TEST_CASE("a map's top row looks north and its middle along the equator", "[formation][pictures]") {
    CHECK(map_direction(0, 0, 256, 128).z > 0.99);
    CHECK(map_direction(10, 127, 256, 128).z < -0.99);
    CHECK_THAT(map_direction(128, 64, 256, 128).z, WithinAbs(0.0, 0.02));
    CHECK_THAT(glm::length(map_direction(37, 91, 256, 128)), WithinAbs(1.0, 1e-12));
}

TEST_CASE("a ramp runs from its first stop to its last", "[formation][pictures]") {
    const std::array<Rgb, 3> stops{hog::formation_lab::rgb(0, 0, 0),
                                   hog::formation_lab::rgb(100, 100, 100),
                                   hog::formation_lab::rgb(200, 0, 50)};
    CHECK(ramp(0.0, stops).r == 0);
    CHECK(ramp(0.5, stops).g == 100);
    CHECK(ramp(1.0, stops).r == 200);
    CHECK(ramp(2.0, stops).b == 50);
}

TEST_CASE("flat ground is lit as flat", "[formation][pictures]") {
    const Sphere sphere{3, 1};
    const std::vector<double> flat(sphere.size(), 100.0);
    std::uint32_t start = 0;
    const double light = hog::formation_lab::shade(sphere, flat, glm::normalize(DVec3{1, 1, 0.3}),
                                                   100'000.0, 0.01, 1.0, start);
    CHECK_THAT(light, WithinAbs(1.0, 1e-9));
}
