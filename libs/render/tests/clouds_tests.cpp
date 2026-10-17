#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/clouds.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <optional>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::render::cloud_period;
using hog::render::Clouds;
using hog::render::CloudsView;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 48, .height = 27};
constexpr VkFormat colour_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr std::uint32_t map_size = 16;
constexpr double radius = 115'000.0;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};
constexpr DVec3 north{0.0, 1.0, 0.0};

using Texel = std::array<std::uint8_t, 4>;
using Pixels = std::vector<std::uint8_t>;

// Where Vulkan samples each texel of a cube's face from.
DVec3 cube_direction(std::uint32_t face, std::uint32_t x, std::uint32_t y) {
    const double s = ((2.0 * (x + 0.5)) / map_size) - 1.0;
    const double t = ((2.0 * (y + 0.5)) / map_size) - 1.0;
    const std::array faces{DVec3{1.0, -t, -s}, DVec3{-1.0, -t, s}, DVec3{s, 1.0, t},
                           DVec3{s, -1.0, -t}, DVec3{s, -t, 1.0},  DVec3{-s, -t, -1.0}};
    return glm::normalize(faces.at(face));
}

std::vector<std::byte> map_of(const std::function<Texel(DVec3)>& weather) {
    std::vector<std::byte> bytes;
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < map_size; ++y) {
            for (std::uint32_t x = 0; x < map_size; ++x) {
                for (const std::uint8_t channel : weather(cube_direction(face, x, y))) {
                    bytes.push_back(static_cast<std::byte>(channel));
                }
            }
        }
    }
    return bytes;
}

std::vector<std::byte> everywhere(Texel texel) {
    return map_of([&](DVec3) { return texel; });
}

const Texel clear{0, 0, 0, 0};
// Fully covered by 1 mm of cloud.
const Texel overcast{255, 64, 0, 0};
// Half covered.
const Texel broken{128, 24, 0, 0};

struct Scene {
    double eye_height = 2.0;
    DVec3 forward = east;
    double sun_elevation = 60.0; // degrees, towards the east
    std::vector<std::byte> before = everywhere(overcast);
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::optional<std::vector<std::byte>> after = {};
    double faded = 1.0;
    DVec3 moved{0.0};
    // Drawn nearest of all.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::optional<VkRect2D> covered = {};
};

// The clouds alone, over black.
Pixels draw(hog::render::Device& device, const Scene& scene) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    auto clouds = Clouds::create(device, colour_format, map_size);
    REQUIRE(clouds);
    CHECK(clouds->map_bytes() == scene.before.size());

    const WorldPosition eye{up * (radius + scene.eye_height)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, 0.0);
    const double elevation = scene.sun_elevation * std::numbers::pi / 180.0;
    const DVec3 sun = (east * std::cos(elevation)) + (up * std::sin(elevation));
    const DVec3 camera_up = glm::normalize(up - (scene.forward * glm::dot(up, scene.forward)));
    const hog::render::Camera camera{
        .eye = eye, .forward = scene.forward, .up = camera_up, .vertical_fov = 1.2};
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), bend);
    const hog::render::SkyView seen{.camera = camera,
                                    .sun = sun,
                                    .bend = bend,
                                    .exposure = 10.0,
                                    .sky_light =
                                        model.sky_irradiance(scene.eye_height * scale, sun)};
    const CloudsView view{.camera = camera,
                          .bend = bend,
                          .air = sky->bindings(0, *atmosphere),
                          .faded = scene.faded,
                          .moved = scene.moved};

    auto colour = hog::render::make_image(
        device, {.format = colour_format,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(colour);
    auto depth = hog::render::make_image(
        device, {.format = depth_format,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
    REQUIRE(depth);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        CHECK(sky->record_frame(commands, extent, 0, *atmosphere, seen));
        CHECK(clouds->update(commands, 0, scene.before, scene.after.value_or(scene.before)));
        hog::render::transition(commands, colour->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, colour->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        if (scene.covered) {
            const VkClearAttachment nearest{
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .colorAttachment = 0,
                .clearValue = {.depthStencil = {.depth = 1.0F, .stencil = 0}}};
            const VkClearRect rect{.rect = *scene.covered, .baseArrayLayer = 0, .layerCount = 1};
            vkCmdClearAttachments(commands, 1, &nearest, 1, &rect);
        }
        vkCmdEndRendering(commands);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                                use::depth_target, use::sampled);
        hog::render::begin_rendering(commands, colour->view(), extent, std::nullopt);
        clouds->draw(commands, extent, depth->view(), view);
        vkCmdEndRendering(commands);
    }));
    CHECK(device.validation_errors() == 0);
    auto pixels = hog::render::read_pixels(device, *colour, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

int brightness(const Pixels& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t first = 4 * ((std::size_t{y} * extent.width) + x);
    return pixels.at(first) + pixels.at(first + 1) + pixels.at(first + 2);
}

// Of the rows above the horizon, when looking level.
long total(const Pixels& pixels, std::uint32_t rows = extent.height / 2) {
    long sum = 0;
    for (std::uint32_t y = 0; y < rows; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            sum += brightness(pixels, x, y);
        }
    }
    return sum;
}

std::uint32_t lit_pixels(const Pixels& pixels, std::uint32_t rows = extent.height / 2) {
    std::uint32_t lit = 0;
    for (std::uint32_t y = 0; y < rows; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            lit += brightness(pixels, x, y) > 0 ? 1U : 0U;
        }
    }
    return lit;
}

} // namespace

TEST_CASE("the clouds' drift is kept within their period", "[render]") {
    const DVec3 within = hog::render::within_period({-1.0, cloud_period + 5.0, 0.0});
    CHECK(within == DVec3{cloud_period - 1.0, 5.0, 0.0});
}

TEST_CASE("with no cloud in the weather, none is drawn", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Pixels pixels = draw(*device, {.before = everywhere(clear)});
    CHECK(total(pixels, extent.height) == 0);
}

TEST_CASE("an overcast sky is covered in grey cloud", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Pixels pixels = draw(*device, {});
    const std::uint32_t above = extent.height / 2;
    CHECK(lit_pixels(pixels) == extent.width * above);
    // Overhead, it's about as red as it's blue.
    const std::size_t first = std::size_t{4} * (extent.width / 2);
    INFO(int{pixels[first]} << ", " << int{pixels[first + 1]} << ", " << int{pixels[first + 2]});
    CHECK(pixels[first] > 0);
    CHECK(pixels[first + 2] < 2 * pixels[first]);
}

TEST_CASE("clouds are hidden by what's drawn in front of them", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const VkRect2D covered{.offset = {.x = 10, .y = 2}, .extent = {.width = 8, .height = 6}};
    const Pixels pixels = draw(*device, {.covered = covered});
    CHECK(brightness(pixels, 12, 4) == 0);
    CHECK(brightness(pixels, 4, 4) > 0);
}

TEST_CASE("clouds fade from the weather before to the weather after", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Scene fading{.before = everywhere(clear), .after = everywhere(overcast)};
    Scene start = fading;
    start.faded = 0.0;
    Scene halfway = fading;
    halfway.faded = 0.5;
    const long none = total(draw(*device, start));
    const long some = total(draw(*device, halfway));
    const long all = total(draw(*device, fading));
    CHECK(none == 0);
    CHECK(some > 0);
    CHECK(some < all);
}

TEST_CASE("clouds lie where the weather has them", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    // Over the north of the planet only, well away from the eye.
    const auto northern =
        map_of([](DVec3 direction) { return direction.y > 0.2 ? overcast : clear; });
    const Pixels north_view = draw(*device, {.forward = north, .before = northern});
    const Pixels south_view = draw(*device, {.forward = -north, .before = northern});
    CHECK(lit_pixels(north_view) > 0);
    CHECK(lit_pixels(south_view) == 0);
    // Out towards the horizon, beyond where they begin.
    CHECK(brightness(north_view, extent.width / 2, (extent.height / 2) - 1) > 0);
}

TEST_CASE("broken cloud billows, drifts with the wind, and repeats over its period",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Scene still{.before = everywhere(broken)};
    Scene around = still;
    around.moved = DVec3{cloud_period, -2.0 * cloud_period, 0.0};
    Scene drifted = still;
    drifted.moved = DVec3{3000.0, 0.0, 0.0};
    const Pixels pixels = draw(*device, still);
    const std::uint32_t above = extent.height / 2;
    CHECK(lit_pixels(pixels) > 0);
    CHECK(lit_pixels(pixels) < extent.width * above);
    CHECK(draw(*device, around) == pixels);
    CHECK(draw(*device, drifted) != pixels);
}

TEST_CASE("thick cloud is dark beneath and bright on top", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Pixels beneath = draw(*device, {.forward = glm::normalize(east + up)});
    const Pixels on_top =
        draw(*device, {.eye_height = 3000.0, .forward = glm::normalize(east - up)});
    const long under = total(beneath, extent.height);
    const long over = total(on_top, extent.height);
    INFO("beneath " << under << ", on top " << over);
    CHECK(under > 0);
    CHECK(over > 3 * under);
}
