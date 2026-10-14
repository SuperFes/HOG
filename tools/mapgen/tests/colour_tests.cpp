#include <hog/mapgen/colour.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
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

TEST_CASE("colours go to linear light and back", "[mapgen][colour]") {
    using hog::mapgen::to_linear;
    using hog::mapgen::to_srgb;
    CHECK(to_linear({0, 0, 0}) == hog::mapgen::LinearRgb{0.0, 0.0, 0.0});
    CHECK(to_linear({255, 255, 255}).g == Catch::Approx(1.0));
    // Mid grey reflects about a fifth of the light.
    CHECK(to_linear({128, 128, 128}).r == Catch::Approx(0.216).epsilon(0.01));
    for (int value = 0; value < 256; value += 15) {
        const auto channel = static_cast<std::uint8_t>(value);
        const Rgb colour{channel, static_cast<std::uint8_t>(255 - value), 40};
        CHECK(to_srgb(to_linear(colour)) == colour);
    }
    CHECK(to_srgb({2.0, -1.0, 0.5}) == Rgb{255, 0, 188});
}
