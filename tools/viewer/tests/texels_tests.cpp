#include <hog/viewer/texels.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

TEST_CASE("map pixels become opaque RGBA texels", "[viewer]") {
    const hog::mapgen::Image image{.width = 2, .height = 1, .pixels = {1, 2, 3, 40, 50, 60}};
    const std::vector<std::byte> expected{std::byte{1},   std::byte{2},  std::byte{3},
                                          std::byte{255}, std::byte{40}, std::byte{50},
                                          std::byte{60},  std::byte{255}};
    CHECK(hog::viewer::rgba_texels(image) == expected);
    CHECK(hog::viewer::rgba_texels(hog::mapgen::Image{}).empty());
}
