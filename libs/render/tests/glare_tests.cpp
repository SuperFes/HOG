#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/glare.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <optional>
#include <vector>

using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::render::eye_spread;
using hog::render::Glare;
using hog::render::GlareView;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 48, .height = 27};
// Blending into it is supported everywhere, which it isn't for 32-bit floats.
constexpr VkFormat colour_format = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr double degree = std::numbers::pi / 180.0;
constexpr DVec3 forward{1.0, 0.0, 0.0};

// From the middle of the planet's frame, looking along +X with +Z up, at a light `right` degrees
// to the right of straight ahead.
GlareView view_of(double right, double angular_radius = hog::render::sun_angular_radius) {
    return {.camera = {.forward = forward, .up = {0.0, 0.0, 1.0}, .vertical_fov = 1.2},
            .towards = {std::cos(right * degree), -std::sin(right * degree), 0.0},
            .angular_radius = angular_radius,
            .irradiance = DVec3{0.01},
            .exposure = 1.0};
}

DVec3 through(const GlareView& view, std::uint32_t x, std::uint32_t y) {
    const double aspect = static_cast<double>(extent.width) / extent.height;
    const glm::dmat4 eye_from_clip =
        glm::inverse(glm::dmat4{hog::render::view_projection(view.camera, aspect)});
    const glm::dvec4 far =
        eye_from_clip * glm::dvec4{((x + 0.5) * 2.0 / extent.width) - 1.0,
                                   ((y + 0.5) * 2.0 / extent.height) - 1.0, 1.0, 1.0};
    return glm::normalize(DVec3{far} / far.w);
}

// The radiance each pixel's red was tone mapped from.
using Picture = std::vector<double>;

// The glare drawn over black, with depth cleared to the sky's and then, within `covered`, to
// the nearest.
Picture draw(hog::render::Device& device, const GlareView& view,
             std::optional<VkRect2D> covered = std::nullopt) {
    auto glare = Glare::create(device, colour_format);
    REQUIRE(glare);
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
    const auto drawn = device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, colour->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, colour->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        if (covered) {
            const VkClearAttachment nearest{
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .colorAttachment = 0,
                .clearValue = {.depthStencil = {.depth = 1.0F, .stencil = 0}}};
            const VkClearRect rect{.rect = *covered, .baseArrayLayer = 0, .layerCount = 1};
            vkCmdClearAttachments(commands, 1, &nearest, 1, &rect);
        }
        vkCmdEndRendering(commands);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                                use::depth_target, use::sampled);
        hog::render::begin_rendering(commands, colour->view(), extent, std::nullopt);
        glare->draw(commands, extent, depth->view(), view);
        vkCmdEndRendering(commands);
    });
    REQUIRE(drawn);
    const auto bytes =
        hog::render::read_pixels(device, *colour, hog::render::image_use::colour_target);
    REQUIRE(bytes);
    Picture picture(std::size_t{extent.width} * extent.height);
    REQUIRE(bytes->size() == picture.size() * 4 * sizeof(std::uint16_t));
    for (std::size_t at = 0; at < picture.size(); ++at) {
        std::uint16_t red = 0;
        std::memcpy(&red, &bytes->at(at * 4 * sizeof(red)), sizeof(red));
        picture[at] = -std::log1p(-static_cast<double>(glm::unpackHalf1x16(red)));
    }
    return picture;
}

double at(const Picture& picture, std::uint32_t x, std::uint32_t y) {
    return picture.at((std::size_t{y} * extent.width) + x);
}

double total(const Picture& picture) {
    double sum = 0.0;
    for (const double radiance : picture) {
        sum += radiance;
    }
    return sum;
}

// Of light from a point, the share the eye spreads between `inner` and `outer` degrees from it.
double spread_between(double inner, double outer) {
    constexpr int steps = 200'000;
    double share = 0.0;
    for (int step = 0; step < steps; ++step) {
        const double angle = inner + ((outer - inner) * (step + 0.5) / steps);
        // A ring's solid angle, with the spread per steradian.
        share += eye_spread(angle) * 2.0 * std::numbers::pi * std::sin(angle * degree) *
                 ((outer - inner) * degree / steps);
    }
    return share;
}

} // namespace

TEST_CASE("the eye spreads light as disability glare does, about a sixth of it past the sun's disc",
          "[render][glare]") {
    // The CIE's veiling glare, 10 / θ² per steradian with θ in degrees.
    for (const double angle : {10.0, 30.0, 60.0}) {
        INFO(angle << " degrees");
        CHECK_THAT(eye_spread(angle), WithinRel(10.0 / (angle * angle), 0.15));
    }
    CHECK(eye_spread(1.0) > eye_spread(2.0));
    CHECK(eye_spread(2.0) > eye_spread(20.0));
    const double rim = hog::render::sun_angular_radius / degree;
    const double past_disc = spread_between(rim, 180.0);
    CHECK(past_disc > 0.12);
    CHECK(past_disc < 0.22);
    CHECK(spread_between(0.0, 180.0) < 1.0);
}

TEST_CASE("the glare round a light is as the eye spreads it", "[render][gpu][glare]") {
    const auto device = test_device();
    const GlareView view = view_of(10.0);
    const Picture picture = draw(*device, view);
    int checked = 0;
    for (std::uint32_t y = 0; y < extent.height; y += 3) {
        for (std::uint32_t x = 0; x < extent.width; x += 3) {
            const double angle = std::acos(glm::dot(through(view, x, y), view.towards)) / degree;
            if (angle < 1.0) {
                continue;
            }
            INFO(x << ", " << y << ": " << angle << " degrees");
            CHECK_THAT(at(picture, x, y), WithinRel(0.01 * eye_spread(angle), 0.02));
            ++checked;
        }
    }
    CHECK(checked > 100);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the glare is hidden as much as the light's disc is", "[render][gpu][glare]") {
    const auto device = test_device();
    // Straight ahead, with a disc a few pixels across, so its half can be covered.
    const GlareView view = view_of(0.0, 0.1);
    const Picture seen = draw(*device, view);
    REQUIRE(total(seen) > 0.0);
    const Picture hidden =
        draw(*device, view, VkRect2D{.offset = {.x = 0, .y = 0}, .extent = extent});
    CHECK(total(hidden) == 0.0);
    const Picture half =
        draw(*device, view,
             VkRect2D{.offset = {.x = 0, .y = 0},
                      .extent = {.width = extent.width / 2, .height = extent.height}});
    CHECK(total(half) > 0.4 * total(seen));
    CHECK(total(half) < 0.65 * total(seen));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a light just out of view glares in from the edge", "[render][gpu][glare]") {
    const auto device = test_device();
    const GlareView view = view_of(60.0);
    // All that's in view covered, which can't hide what isn't.
    const Picture picture =
        draw(*device, view, VkRect2D{.offset = {.x = 0, .y = 0}, .extent = extent});
    const std::uint32_t edge = extent.width - 1;
    const std::uint32_t middle = extent.height / 2;
    const double angle = std::acos(glm::dot(through(view, edge, middle), view.towards)) / degree;
    CHECK_THAT(at(picture, edge, middle), WithinRel(0.01 * eye_spread(angle), 0.02));
    CHECK(at(picture, edge, middle) > at(picture, 0, middle));
    CHECK(device->validation_errors() == 0);
}
