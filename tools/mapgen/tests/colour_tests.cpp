#include <hog/mapgen/colour.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

using hog::mapgen::ColourStop;
using hog::mapgen::Rgb;

namespace {

constexpr std::array stops{
    ColourStop{.at = 0.0, .colour = {0, 0, 0}},
    ColourStop{.at = 10.0, .colour = {200, 100, 50}},
    ColourStop{.at = 20.0, .colour = {200, 200, 250}},
};

} // namespace

TEST_CASE("a ramp blends between the stops around a value", "[mapgen][colour]") {
    CHECK(hog::mapgen::ramp(stops, 0.0) == Rgb{0, 0, 0});
    CHECK(hog::mapgen::ramp(stops, 5.0) == Rgb{100, 50, 25});
    CHECK(hog::mapgen::ramp(stops, 10.0) == Rgb{200, 100, 50});
    CHECK(hog::mapgen::ramp(stops, 15.0) == Rgb{200, 150, 150});
}

TEST_CASE("a ramp holds its end colours beyond its stops", "[mapgen][colour]") {
    CHECK(hog::mapgen::ramp(stops, -5.0) == Rgb{0, 0, 0});
    CHECK(hog::mapgen::ramp(stops, 99.0) == Rgb{200, 200, 250});
    CHECK(hog::mapgen::ramp(stops, std::numeric_limits<double>::infinity()) == Rgb{200, 200, 250});
}

TEST_CASE("mixing moves one colour towards another", "[mapgen][colour]") {
    const Rgb black{0, 0, 0};
    const Rgb white{255, 255, 255};
    CHECK(hog::mapgen::mix(black, white, 0.0) == black);
    CHECK(hog::mapgen::mix(black, white, 1.0) == white);
    CHECK(hog::mapgen::mix(black, white, 0.5) == Rgb{128, 128, 128});
    CHECK(hog::mapgen::mix(black, white, -1.0) == black);
    CHECK(hog::mapgen::mix(black, white, 2.0) == white);
}
