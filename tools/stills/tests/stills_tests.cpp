#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/stills/stills.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

TEST_CASE("the map shows the sea blue and the land not", "[stills]") {
    const hog::globe::Generator earth{hog::globe::preset("earth").value_or(hog::globe::Planet{})};
    const hog::stills::Image map = hog::stills::map(earth, 64, 2);
    REQUIRE(map.width == 64);
    REQUIRE(map.height == 32);
    REQUIRE(map.pixels.size() == std::size_t{64} * 32 * 3);
    int blue = 0;
    for (int y = 0; y < map.height; ++y) {
        for (int x = 0; x < map.width; ++x) {
            const auto [r, g, b] = map.at(x, y);
            blue += b > r + 40 ? 1 : 0;
        }
    }
    // Some 70% under the sea, less where the poles stretch the land.
    CHECK(blue > 64 * 32 / 2);
    CHECK(blue < 64 * 32 * 9 / 10);
}

TEST_CASE("a section shows air over the ground and rock under it", "[stills]") {
    const hog::globe::Generator moon{hog::globe::preset("moon").value_or(hog::globe::Planet{})};
    const hog::stills::Place place{.latitude = 10.0, .longitude = 20.0};
    const hog::stills::Image cut =
        hog::stills::section(moon, place, 8'000.0, -8'000.0, 8'000.0, 40, 2);
    REQUIRE(cut.width == 40);
    REQUIRE(cut.height == 80);
    CHECK(cut.at(20, 0) == std::array<std::uint8_t, 3>{205, 225, 250});
    CHECK(cut.at(20, 79) != std::array<std::uint8_t, 3>{205, 225, 250});
    const hog::stills::Image square = hog::stills::relief(moon, place, 5'000.0, 16, 2);
    CHECK(square.width == 16);
    CHECK(square.height == 16);
}
