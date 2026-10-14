#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/globe.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/orbit.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/streamed_image.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using hog::render::testing::test_device;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

constexpr double radius = 1'000.0;
constexpr VkExtent2D extent{.width = 32, .height = 32};
// One per face, in face order: +x, +y, +z, -x, -y, -z.
constexpr std::array<Pixel, 6> face_colours{{{200, 0, 0, 255},
                                             {0, 200, 0, 255},
                                             {0, 0, 200, 255},
                                             {200, 200, 0, 255},
                                             {0, 200, 200, 255},
                                             {200, 0, 200, 255}}};

hog::render::StreamedImage coloured_faces(hog::render::Device& device) {
    constexpr VkExtent2D face{.width = 2, .height = 2};
    auto faces = hog::render::make_streamed_image(device, VK_FORMAT_R8G8B8A8_UNORM, face, 1, 6);
    REQUIRE(faces);
    std::vector<std::byte> texels;
    for (const Pixel& colour : face_colours) {
        for (std::uint32_t texel = 0; texel < face.width * face.height; ++texel) {
            for (const std::uint8_t channel : colour) {
                texels.push_back(static_cast<std::byte>(channel));
            }
        }
    }
    REQUIRE(device.submit_now(
        [&](VkCommandBuffer commands) { REQUIRE(faces->update(commands, 0, texels)); }));
    return std::move(faces).value();
}

// Draws the globe over a black background and reads it back.
std::vector<std::uint8_t> draw(hog::render::Device& device, const hog::render::Globe& globe,
                               const hog::render::Image& faces,
                               const hog::render::GlobeView& view) {
    auto target = hog::render::make_image(
        device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(target);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::begin_rendering(commands, target->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}});
        globe.draw(commands, extent, faces, view);
        vkCmdEndRendering(commands);
    }));
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

Pixel pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t first = 4 * ((std::size_t{y} * extent.width) + x);
    return {pixels.at(first), pixels.at(first + 1), pixels.at(first + 2), pixels.at(first + 3)};
}

hog::render::GlobeView view_from(double latitude, double longitude, hog::DVec3 sun) {
    return {
        .camera = hog::render::orbit_camera(
            {.latitude = latitude, .longitude = longitude, .altitude = 2.0 * radius}, radius, 1.0),
        .radius = radius,
        .sun = sun,
        .night_light = 0.25,
        .twilight = 0.1};
}

} // namespace

TEST_CASE("the globe shows the face it's seen from, over the background", "[render][gpu]") {
    const auto device = test_device();
    auto globe = hog::render::Globe::create(*device, VK_FORMAT_R8G8B8A8_UNORM, 8);
    REQUIRE(globe);
    const auto faces = coloured_faces(*device);

    const auto from_x = draw(*device, *globe, faces.image(), view_from(0.0, 0.0, {1.0, 0.0, 0.0}));
    CHECK(pixel_at(from_x, 16, 16) == face_colours[0]);
    CHECK(pixel_at(from_x, 1, 1) == Pixel{0, 0, 0, 255});
    CHECK(pixel_at(from_x, 30, 30) == Pixel{0, 0, 0, 255});

    const auto from_north =
        draw(*device, *globe, faces.image(), view_from(1.5, 0.0, {0.0, 0.0, 1.0}));
    CHECK(pixel_at(from_north, 16, 16) == face_colours[2]);

    const auto from_minus_y =
        draw(*device, *globe, faces.image(), view_from(0.0, -1.5, {0.0, -1.0, 0.0}));
    CHECK(pixel_at(from_minus_y, 16, 16) == face_colours[4]);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the globe's night side stays dimly lit", "[render][gpu]") {
    const auto device = test_device();
    auto globe = hog::render::Globe::create(*device, VK_FORMAT_R8G8B8A8_UNORM, 8);
    REQUIRE(globe);
    const auto faces = coloured_faces(*device);

    const auto night = draw(*device, *globe, faces.image(), view_from(0.0, 0.0, {-1.0, 0.0, 0.0}));
    CHECK(pixel_at(night, 16, 16) == Pixel{50, 0, 0, 255});
    CHECK(device->validation_errors() == 0);
}
