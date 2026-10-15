#include <hog/client/likeness.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstddef>
#include <cstdint>

using Catch::Matchers::WithinAbs;
using hog::client::Picture;

namespace {

// 10×10 of one grey.
Picture grey(int value) {
    return {.width = 10,
            .height = 10,
            .texels = std::vector<std::byte>(400, static_cast<std::byte>(value))};
}

void set(Picture& picture, std::size_t pixel, std::size_t channel, int value) {
    picture.texels.at((pixel * 4) + channel) = static_cast<std::byte>(value);
}

int grey_at(const Picture& picture, std::size_t pixel) {
    return std::to_integer<int>(picture.texels.at(pixel * 4));
}

} // namespace

TEST_CASE("a picture is like itself", "[client][likeness]") {
    const auto found = hog::client::difference(grey(100), grey(100));
    REQUIRE(found);
    CHECK(found->mean == 0.0);
    CHECK(found->share == 0.0);
    CHECK(hog::client::alike(*found));
    CHECK(grey_at(found->picture, 37) == 0);
}

TEST_CASE("pictures differ by each pixel's largest channel difference, not by alpha",
          "[client][likeness]") {
    Picture drawn = grey(100);
    set(drawn, 0, 0, 110); // red 10 over
    set(drawn, 0, 2, 96);  // blue 4 under
    set(drawn, 1, 3, 0);   // alpha only
    const auto found = hog::client::difference(grey(100), drawn);
    REQUIRE(found);
    CHECK_THAT(found->mean, WithinAbs(0.1, 1e-12));
    CHECK_THAT(found->share, WithinAbs(0.01, 1e-12));
    CHECK(grey_at(found->picture, 0) == 80);
    CHECK(grey_at(found->picture, 1) == 0);
}

TEST_CASE("pictures alike allow small differences everywhere and large ones in few places",
          "[client][likeness]") {
    const auto slightly = hog::client::difference(grey(100), grey(101));
    REQUIRE(slightly);
    CHECK(hog::client::alike(*slightly));
    CHECK(!hog::client::alike(*slightly, {.mean = 0.5}));

    const auto everywhere = hog::client::difference(grey(100), grey(103));
    REQUIRE(everywhere);
    CHECK(!hog::client::alike(*everywhere));

    Picture spot = grey(100);
    set(spot, 5, 1, 200);
    const auto one = hog::client::difference(grey(100), spot);
    REQUIRE(one);
    CHECK(hog::client::alike(*one));
    set(spot, 6, 1, 200);
    const auto two = hog::client::difference(grey(100), spot);
    REQUIRE(two);
    CHECK(!hog::client::alike(*two));
    CHECK(grey_at(two->picture, 6) == 255);
}

TEST_CASE("pictures of different sizes can't be compared", "[client][likeness]") {
    Picture wider = grey(100);
    wider.width = 20;
    wider.height = 5;
    CHECK(!hog::client::difference(grey(100), wider));
    Picture short_of_texels = grey(100);
    short_of_texels.texels.resize(396);
    CHECK(!hog::client::difference(grey(100), short_of_texels));
}
