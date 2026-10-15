#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/billboard.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::render::Camera;
using hog::render::Standing;
using hog::render::testing::test_device;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

constexpr VkExtent2D extent{.width = 64, .height = 64};
constexpr Pixel background{0, 0, 0, 255};
constexpr Pixel red{255, 0, 0, 255};

// Two texels by two, red but for the clear one at the top left.
hog::render::Billboard make_billboard(hog::render::Device& device) {
    std::vector<std::byte> texels;
    for (const Pixel texel : {Pixel{0, 0, 0, 0}, red, red, red}) {
        for (const std::uint8_t value : texel) {
            texels.push_back(static_cast<std::byte>(value));
        }
    }
    auto billboard = hog::render::Billboard::create(
        device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, {.width = 2, .height = 2}, texels);
    REQUIRE(billboard);
    return std::move(billboard).value();
}

// Draws over a black background and reads it back.
std::vector<std::uint8_t> draw(hog::render::Device& device, const hog::render::Billboard& billboard,
                               const Camera& camera, const Standing& standing,
                               const hog::terrain::Bend& bend = {}) {
    auto target = hog::render::make_image(
        device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(target);
    auto depth =
        hog::render::make_image(device, {.format = VK_FORMAT_D32_SFLOAT,
                                         .extent = extent,
                                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
    REQUIRE(depth);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, target->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        billboard.draw(commands, extent, camera, standing, bend);
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

} // namespace

TEST_CASE("a billboard stands upright where it's put, see-through where its picture is",
          "[render][gpu]") {
    const auto device = test_device();
    const auto billboard = make_billboard(*device);
    // Two metres tall and wide, from a metre below the eye, four metres ahead.
    const Camera camera{.forward = {0.0, 0.0, -1.0}, .up = {0.0, 1.0, 0.0}};
    const auto pixels =
        draw(*device, billboard, camera,
             {.feet = WorldPosition{0.0, -1.0, -4.0}, .up = {0.0, 1.0, 0.0}, .height = 2.0});
    CHECK(pixel_at(pixels, 40, 40) == red);
    CHECK(pixel_at(pixels, 40, 24) == red);
    CHECK(pixel_at(pixels, 24, 40) == red);
    CHECK(pixel_at(pixels, 24, 24) == background);
    CHECK(pixel_at(pixels, 4, 32) == background);
    CHECK(pixel_at(pixels, 32, 4) == background);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a billboard turns to face the eye", "[render][gpu]") {
    const auto device = test_device();
    const auto billboard = make_billboard(*device);
    const Standing ahead{
        .feet = WorldPosition{0.0, -1.0, -4.0}, .up = {0.0, 1.0, 0.0}, .height = 2.0};
    const auto straight =
        draw(*device, billboard, {.forward = {0.0, 0.0, -1.0}, .up = {0.0, 1.0, 0.0}}, ahead);
    // The same, seen from a quarter turn round it.
    const auto beside = draw(
        *device, billboard,
        {.eye = WorldPosition{-4.0, 0.0, -4.0}, .forward = {1.0, 0.0, 0.0}, .up = {0.0, 1.0, 0.0}},
        ahead);
    CHECK(beside == straight);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("bent, a billboard stands where the ground under it is drawn", "[render][gpu]") {
    const auto device = test_device();
    const auto billboard = make_billboard(*device);
    constexpr double radius = 115'000.0;
    constexpr double along = 6'000.0;
    const DVec3 up{std::sin(along / radius), 0.0, std::cos(along / radius)};
    const Standing far{.feet = WorldPosition{up * radius}, .up = up, .height = 60.0};
    // Looking east along the ground from three metres up, 0.1 radians high.
    const Camera camera{.eye = WorldPosition{0.0, 0.0, radius + 3.0},
                        .forward = {1.0, 0.0, 0.0},
                        .up = {0.0, 0.0, 1.0},
                        .vertical_fov = 0.1};
    const auto feet_row = [&](const hog::terrain::Bend& bend) {
        const auto pixels = draw(*device, billboard, camera, far, bend);
        double lowest = 0.0;
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            if (pixel_at(pixels, extent.width / 2, y) == red) {
                lowest = static_cast<double>(y) + 1.0;
            }
        }
        return lowest;
    };
    const auto drawn_row = [&](DVec3 from_eye) {
        const glm::vec4 clip =
            hog::render::view_projection(camera, 1.0) * glm::vec4{hog::Vec3{from_eye}, 1.0F};
        return (static_cast<double>(clip.y / clip.w) + 1.0) / 2.0 * extent.height;
    };
    const DVec3 feet = far.feet - camera.eye;
    const hog::terrain::Bend bend = hog::terrain::bend_from(camera.eye, radius, 0.0);
    const double unbent = feet_row({});
    const double bent = feet_row(bend);
    CHECK(std::abs(unbent - drawn_row(feet)) < 1.0);
    CHECK(std::abs(bent - drawn_row(hog::terrain::bent(bend, feet))) < 1.0);
    CHECK(unbent - bent > 10.0);
    CHECK(device->validation_errors() == 0);
}
