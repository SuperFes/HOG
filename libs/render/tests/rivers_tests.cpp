#include "test_device.hpp"

#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/rivers.hpp>
#include <hog/render/sky.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/river_water.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::render::Camera;
using hog::render::testing::test_device;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

constexpr VkExtent2D extent{.width = 64, .height = 64};
constexpr double earth = 6'371'000.0;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};
constexpr DVec3 north{0.0, 1.0, 0.0};

// A river four metres wide flowing east at sea level, from five metres east of the point under
// the eye to fifty, laid out over its banks either side.
hog::terrain::RiverWater river() {
    hog::terrain::RiverWater water{
        .origin = WorldPosition{up * earth}, .vertices = {}, .indices = {}};
    for (const double along : {5.0, 50.0}) {
        for (const float across : {-2.0F, -1.0F, 0.0F, 1.0F, 2.0F}) {
            water.vertices.push_back(
                {.position =
                     hog::Vec3{(east * along) + (north * (2.0 * static_cast<double>(across)))},
                 .beyond = 2.0F * (std::abs(across) - 1.0F),
                 .flow = hog::Vec3{east},
                 .rough = 0.0F});
        }
    }
    for (std::uint32_t corner = 0; corner < 4; ++corner) {
        water.indices.insert(water.indices.end(),
                             {corner, corner + 5, corner + 1, corner + 1, corner + 5, corner + 6});
    }
    return water;
}

// From three metres over the river's level, looking east along it and down.
Camera looking() {
    const DVec3 forward = glm::normalize(east - (up * 0.3));
    return {.eye = WorldPosition{up * (earth + 3.0)},
            .forward = forward,
            .up = glm::cross(glm::cross(forward, up), forward)};
}

// As the picture keeps it, the depth of the bed `under` metres beneath the river's surface, seen
// through the middle of the picture.
float bed_depth(double under) {
    const Camera camera = looking();
    const double down = -glm::dot(camera.forward, up);
    const DVec3 bed = camera.forward * ((3.0 + under) / down);
    const glm::vec4 clip =
        hog::render::view_projection(camera, 1.0) * glm::vec4{hog::Vec3{bed}, 1.0F};
    return clip.z / clip.w;
}

// What's drawn beneath the water, as copied: all of `colour`, at `depth` as the copy keeps it.
struct Beneath {
    hog::render::Image colour;
    hog::render::Image depth;
};

Beneath beneath(hog::render::Device& device, std::array<float, 4> colour, float depth) {
    auto colours = hog::render::make_image(
        device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
    REQUIRE(colours);
    auto depths = hog::render::make_image(
        device, {.format = VK_FORMAT_D32_SFLOAT,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
    REQUIRE(depths);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, colours->handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                use::discard, use::copy_target);
        hog::render::transition(commands, depths->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::copy_target);
        const VkClearColorValue cleared{.float32 = {colour[0], colour[1], colour[2], colour[3]}};
        const VkImageSubresourceRange colour_range{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                   .baseMipLevel = 0,
                                                   .levelCount = 1,
                                                   .baseArrayLayer = 0,
                                                   .layerCount = 1};
        vkCmdClearColorImage(commands, colours->handle(), use::copy_target.layout, &cleared, 1,
                             &colour_range);
        const VkClearDepthStencilValue deep{.depth = depth, .stencil = 0};
        VkImageSubresourceRange depth_range = colour_range;
        depth_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdClearDepthStencilImage(commands, depths->handle(), use::copy_target.layout, &deep, 1,
                                    &depth_range);
        hog::render::transition(commands, colours->handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                use::copy_target, use::sampled);
        hog::render::transition(commands, depths->handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                                use::copy_target, use::sampled);
    }));
    return {.colour = std::move(colours).value(), .depth = std::move(depths).value()};
}

// As looking() sees it at noon, over a black background, seeing through to what's beneath, as
// copied.
std::vector<std::uint8_t> drawn(hog::render::Device& device, const Beneath& under) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(sky);
    auto rivers =
        hog::render::RiverSurface::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(rivers);
    rivers->show(std::make_shared<const hog::terrain::RiverWater>(river()));
    const DVec3 sun = glm::normalize(up + (north * 0.3));
    const Camera camera = looking();
    const hog::terrain::Bend bend = hog::terrain::bend_from(camera.eye, earth, 0.0);
    const hog::render::SkyView seen{.camera = camera, .sun = sun, .bend = bend, .exposure = 10.0};

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
        CHECK(sky->record_frame(commands, extent, 0, *atmosphere, seen));
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, target->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        CHECK(rivers->draw(commands, extent,
                           {.camera = camera,
                            .sun = sun,
                            .bend = bend,
                            .air = sky->bindings(0, *atmosphere),
                            .behind = hog::render::SeaBehind{.colour = under.colour.view(),
                                                             .depth = under.depth.view(),
                                                             .environment = VK_NULL_HANDLE},
                            .time = 10.0},
                           0));
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

int brightness(Pixel pixel) {
    return pixel[0] + pixel[1] + pixel[2];
}

} // namespace

TEST_CASE("rivers' water is drawn where it lies and nowhere else", "[render][gpu]") {
    const auto device = test_device();
    const auto pixels = drawn(*device, beneath(*device, {0.0F, 0.0F, 0.0F, 1.0F}, 0.0F));
    // Ahead along it, and off to either side of it.
    CHECK(brightness(pixel_at(pixels, 32, 32)) > 0);
    CHECK(brightness(pixel_at(pixels, 1, 32)) == 0);
    CHECK(brightness(pixel_at(pixels, 62, 32)) == 0);
    // Above the horizon, where it never reaches.
    CHECK(brightness(pixel_at(pixels, 32, 4)) == 0);
}

TEST_CASE("rivers' water is seen through to what's beneath it", "[render][gpu]") {
    const auto device = test_device();
    // A hand under the surface.
    const float bed = bed_depth(0.3);
    const Pixel red =
        pixel_at(drawn(*device, beneath(*device, {1.0F, 0.0F, 0.0F, 1.0F}, bed)), 32, 32);
    const Pixel green =
        pixel_at(drawn(*device, beneath(*device, {0.0F, 1.0F, 0.0F, 1.0F}, bed)), 32, 32);
    CHECK(red[0] > green[0]);
    CHECK(green[1] > red[1]);
}
