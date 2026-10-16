#include "test_device.hpp"

#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/capsules.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::render::Camera;
using hog::render::Capsule;
using hog::render::CapsuleView;
using hog::render::testing::test_device;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

constexpr VkExtent2D extent{.width = 64, .height = 64};
constexpr Pixel background{0, 0, 0, 255};

// Two metres tall and a metre wide, from a metre below the eye, four metres ahead.
Capsule ahead() {
    return {.feet = WorldPosition{0.0, -1.0, -4.0},
            .up = {0.0, 1.0, 0.0},
            .height = 2.0,
            .radius = 0.5,
            .colour = hog::Vec3{1.0F}};
}

Camera looking() {
    return {.forward = {0.0, 0.0, -1.0}, .up = {0.0, 1.0, 0.0}};
}

// Draws over a black background and reads it back.
std::vector<std::uint8_t> draw(hog::render::Device& device, const CapsuleView& view,
                               const std::vector<Capsule>& capsules) {
    auto drawer =
        hog::render::Capsules::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(drawer);
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
        drawer->draw(commands, extent, view, capsules);
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

constexpr double earth = 6'371'000.0;

// From two metres over the sea of an Earth-sized planet, looking level east at a pale capsule
// ten metres away, lit through the air with the sun `sun_elevation` degrees up behind the eye.
std::vector<std::uint8_t> through_air(hog::render::Device& device, double sun_elevation) {
    const DVec3 up{0.0, 0.0, 1.0};
    const DVec3 east{1.0, 0.0, 0.0};
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(sky);
    auto drawer =
        hog::render::Capsules::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, true);
    REQUIRE(drawer);
    const WorldPosition eye{up * (earth + 2.0)};
    const double elevation = sun_elevation * std::numbers::pi / 180.0;
    const DVec3 sun = (-east * std::cos(elevation)) + (up * std::sin(elevation));
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, earth, 0.0);
    const Camera camera{.eye = eye, .forward = east, .up = up};
    const hog::render::SkyView seen{.camera = camera, .sun = sun, .bend = bend, .exposure = 10.0};
    const CapsuleView view{
        .camera = camera, .sun = sun, .bend = bend, .air = sky->bindings(0, *atmosphere)};
    const std::vector<Capsule> capsules{
        {.feet = WorldPosition{(up * earth) + (east * 10.0)}, .up = up, .colour = hog::Vec3{0.8F}}};

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
        drawer->draw(commands, extent, view, capsules);
        vkCmdEndRendering(commands);
    }));
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

} // namespace

TEST_CASE("a capsule stands upright where it's put", "[render][gpu]") {
    const auto device = test_device();
    const auto pixels = draw(*device, {.camera = looking()}, {ahead()});
    CHECK(pixel_at(pixels, 32, 32) != background);
    CHECK(pixel_at(pixels, 32, 22) != background);
    CHECK(pixel_at(pixels, 32, 42) != background);
    // A metre wide is about fifteen pixels at four metres.
    CHECK(pixel_at(pixels, 20, 32) == background);
    CHECK(pixel_at(pixels, 44, 32) == background);
    CHECK(pixel_at(pixels, 32, 8) == background);
    CHECK(pixel_at(pixels, 32, 56) == background);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a capsule is lit on the side facing the sun", "[render][gpu]") {
    const auto device = test_device();
    const auto pixels =
        draw(*device, {.camera = looking(), .sun = {1.0, 0.0, 0.0}, .night = 0.2}, {ahead()});
    const Pixel away = pixel_at(pixels, 27, 32);
    CHECK(brightness(pixel_at(pixels, 37, 32)) > brightness(away) + 150);
    // Facing away, only as bright as the night leaves it.
    CHECK(std::abs(static_cast<int>(away[0]) - 51) <= 2);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a capsule hides another behind it, whichever is drawn first", "[render][gpu]") {
    const auto device = test_device();
    Capsule behind = ahead();
    behind.feet = WorldPosition{0.0, -1.0, -8.0};
    behind.colour = {1.0F, 0.0F, 0.0F};
    Capsule front = ahead();
    front.colour = {0.0F, 0.0F, 1.0F};
    const CapsuleView view{.camera = looking(), .sun = {0.0, 0.0, 1.0}};
    for (const auto& capsules : {std::vector{behind, front}, std::vector{front, behind}}) {
        const Pixel middle = pixel_at(draw(*device, view, capsules), 32, 32);
        CHECK(middle[0] == 0);
        CHECK(middle[2] > 200);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("bent, a capsule stands where the ground under it is drawn", "[render][gpu]") {
    const auto device = test_device();
    constexpr double radius = 115'000.0;
    constexpr double along = 6'000.0;
    const DVec3 up{std::sin(along / radius), 0.0, std::cos(along / radius)};
    const Capsule far{.feet = WorldPosition{up * radius}, .up = up, .height = 60.0, .radius = 10.0};
    // Looking east along the ground from three metres up, 0.1 radians high.
    const Camera camera{.eye = WorldPosition{0.0, 0.0, radius + 3.0},
                        .forward = {1.0, 0.0, 0.0},
                        .up = {0.0, 0.0, 1.0},
                        .vertical_fov = 0.1};
    const auto feet_row = [&](const hog::terrain::Bend& bend) {
        const auto pixels = draw(*device, {.camera = camera, .bend = bend}, {far});
        double lowest = 0.0;
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            if (pixel_at(pixels, extent.width / 2, y) != background) {
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
    CHECK(std::abs(unbent - drawn_row(feet)) < 1.5);
    CHECK(std::abs(bent - drawn_row(hog::terrain::bent(bend, feet))) < 1.5);
    CHECK(unbent - bent > 10.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("through the air, a capsule is lit by day and dark by night", "[render][gpu]") {
    const auto device = test_device();
    const auto day = through_air(*device, 30.0);
    const auto night = through_air(*device, -30.0);
    CHECK(brightness(pixel_at(day, 32, 38)) > 200);
    CHECK(brightness(pixel_at(night, 32, 38)) < 30);
    CHECK(device->validation_errors() == 0);
}
