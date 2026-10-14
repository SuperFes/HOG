#include "test_device.hpp"

#include <hog/render/readback.hpp>
#include <hog/render/streamed_image.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using hog::render::make_streamed_image;
using hog::render::testing::test_device;

namespace {

std::vector<std::byte> filled(std::size_t size, std::uint8_t first) {
    std::vector<std::byte> texels(size);
    for (std::size_t at = 0; at < size; ++at) {
        texels[at] = static_cast<std::byte>(first + at);
    }
    return texels;
}

} // namespace

TEST_CASE("only 4-byte colour formats are streamed", "[render]") {
    CHECK(hog::render::texel_bytes(VK_FORMAT_R8G8B8A8_UNORM) == 4U);
    CHECK(hog::render::texel_bytes(VK_FORMAT_B8G8R8A8_SRGB) == 4U);
    CHECK_FALSE(hog::render::texel_bytes(VK_FORMAT_R8G8B8_UNORM).has_value());
    CHECK_FALSE(hog::render::texel_bytes(VK_FORMAT_D32_SFLOAT).has_value());
}

TEST_CASE("read-back pixels lose their alpha and come out red first", "[render]") {
    const std::vector<std::uint8_t> two{1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(
        hog::render::to_rgb(two, VK_FORMAT_R8G8B8A8_UNORM).value_or(std::vector<std::uint8_t>{}) ==
        std::vector<std::uint8_t>{1, 2, 3, 5, 6, 7});
    CHECK(hog::render::to_rgb(two, VK_FORMAT_B8G8R8A8_SRGB).value_or(std::vector<std::uint8_t>{}) ==
          std::vector<std::uint8_t>{3, 2, 1, 7, 6, 5});
    CHECK(hog::render::to_rgb(two, VK_FORMAT_R16G16B16A16_SFLOAT).error().code ==
          hog::ErrorCode::Unsupported);
}

TEST_CASE("a streamed image takes each upload whole and refuses the wrong size", "[render][gpu]") {
    const auto device = test_device();
    constexpr VkExtent2D extent{.width = 4, .height = 2};
    constexpr std::size_t size = std::size_t{4} * 4 * 2;
    CHECK(make_streamed_image(*device, VK_FORMAT_R8G8B8_UNORM, extent, 2).error().code ==
          hog::ErrorCode::Unsupported);
    CHECK(make_streamed_image(*device, VK_FORMAT_R8G8B8A8_UNORM, {.width = 0, .height = 2}, 2)
              .error()
              .code == hog::ErrorCode::InvalidArgument);

    auto streamed = make_streamed_image(*device, VK_FORMAT_R8G8B8A8_UNORM, extent, 2);
    REQUIRE(streamed);
    CHECK_FALSE(streamed->ready());
    const std::vector<std::byte> first = filled(size, 0);
    const std::vector<std::byte> second = filled(size, 100);
    REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
        CHECK(streamed->update(commands, 0, first));
        CHECK(streamed->update(commands, 2, first).error().code == hog::ErrorCode::InvalidArgument);
        CHECK(streamed->update(commands, 1, std::span{first}.first(size - 4)).error().code ==
              hog::ErrorCode::InvalidArgument);
    }));
    CHECK(streamed->ready());
    REQUIRE(device->submit_now(
        [&](VkCommandBuffer commands) { CHECK(streamed->update(commands, 1, second)); }));

    const auto pixels =
        hog::render::read_pixels(*device, streamed->image(), hog::render::image_use::sampled);
    REQUIRE(pixels);
    REQUIRE(pixels->size() == size);
    for (std::size_t at = 0; at < size; ++at) {
        CHECK(static_cast<std::byte>(pixels->at(at)) == second[at]);
    }
    CHECK(device->validation_errors() == 0);
}
