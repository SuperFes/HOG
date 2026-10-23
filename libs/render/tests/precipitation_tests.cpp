#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/precipitation.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::WorldPosition;
using hog::render::among;
using hog::render::fall_velocity;
using hog::render::Precipitation;
using hog::render::precipitation_count;
using hog::render::precipitation_reach;
using hog::render::PrecipitationView;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 96, .height = 54};
constexpr VkFormat colour_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr double radius = 115'000.0;
constexpr double box = 2.0 * precipitation_reach;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};

using Pixels = std::vector<std::uint8_t>;

struct Scene {
    double falling = 5.0;
    double snow = 0.0;
    double seconds = 0.0;
    DVec3 wind{0.0};
    double depth = 0.0;  // cleared to, nearer the greater
    double height = 2.0; // metres over a calm sea
};

// What falls, alone over black, seen from `height` looking level east by day.
Pixels draw(hog::render::Device& device, const Scene& scene) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    auto precipitation = Precipitation::create(device, colour_format, depth_format);
    REQUIRE(precipitation);
    auto ocean = hog::render::Ocean::create(device, colour_format, depth_format, true);
    REQUIRE(ocean);

    const WorldPosition eye{up * (radius + scene.height)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, 0.0);
    const double elevation = 40.0 * std::numbers::pi / 180.0;
    const DVec3 sun = (east * std::cos(elevation)) + (up * std::sin(elevation));
    const hog::render::Camera camera{.eye = eye, .forward = east, .up = up, .vertical_fov = 1.2};
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), bend);
    const hog::render::SkyView seen{.camera = camera,
                                    .sun = sun,
                                    .bend = bend,
                                    .exposure = 10.0,
                                    .sky_light = model.sky_irradiance(2.0 * scale, sun)};
    const PrecipitationView view{.camera = camera,
                                 .air = sky->bindings(0, *atmosphere),
                                 .up = up,
                                 .wind = scene.wind,
                                 .falling = scene.falling,
                                 .snow = scene.snow,
                                 .seconds = scene.seconds};
    const hog::render::OceanView sea{.camera = camera, .sun = sun, .bend = bend, .air = view.air};

    auto colour = hog::render::make_image(
        device, {.format = colour_format,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(colour);
    auto depth =
        hog::render::make_image(device, {.format = depth_format,
                                         .extent = extent,
                                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
    REQUIRE(depth);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        CHECK(sky->record_frame(commands, extent, 0, *atmosphere, seen));
        CHECK(ocean->lay_water(commands, sea, 0, true));
        hog::render::transition(commands, colour->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(
            commands, colour->view(), extent,
            VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
            {.view = depth->view(), .clear = static_cast<float>(scene.depth)});
        precipitation->draw(commands, extent, view);
        vkCmdEndRendering(commands);
    }));
    CHECK(device.validation_errors() == 0);
    auto pixels = hog::render::read_pixels(device, *colour, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

int brightness(const Pixels& pixels, std::size_t pixel) {
    return pixels.at(4 * pixel) + pixels.at((4 * pixel) + 1) + pixels.at((4 * pixel) + 2);
}

std::size_t lit_pixels(const Pixels& pixels) {
    std::size_t lit = 0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
        lit += brightness(pixels, pixel) > 0 ? 1U : 0U;
    }
    return lit;
}

// Of the pixels drawn on, how bright they are on average.
double mean_lit(const Pixels& pixels) {
    long sum = 0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
        sum += brightness(pixels, pixel);
    }
    const std::size_t lit = lit_pixels(pixels);
    return lit == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(lit);
}

} // namespace

TEST_CASE("more falls the heavier it rains, and more again as snow", "[render]") {
    CHECK(precipitation_count(0.0, 0.0) == 0);
    CHECK(precipitation_count(-1.0, 0.0) == 0);
    CHECK(precipitation_count(0.5, 0.0) > 0);
    CHECK(precipitation_count(2.0, 0.0) > precipitation_count(1.0, 0.0));
    CHECK(precipitation_count(1.0, 1.0) > precipitation_count(1.0, 0.0));
    CHECK(precipitation_count(1000.0, 0.0) == hog::render::most_precipitation);
}

TEST_CASE("rain and snow fall down, snow slower, both blown along the ground", "[render]") {
    // The wind's upward part is ignored.
    const DVec3 wind{3.0, -2.0, 5.0};
    const DVec3 rain = fall_velocity(up, wind, false);
    const DVec3 snow = fall_velocity(up, wind, true);
    CHECK(rain == DVec3{3.0, -2.0, -hog::render::rain_speed});
    CHECK(snow == DVec3{3.0, -2.0, -hog::render::snow_speed});
}

TEST_CASE("the eye is among what falls as if it repeated every box", "[render]") {
    const WorldPosition eye{radius + 3.5, -7.25, 1.0};
    const DVec3 velocity{1.0, 0.0, -7.0};
    const DVec3 at = among(eye, velocity, 0.0);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        CHECK(at[axis] >= 0.0);
        CHECK(at[axis] < box);
    }
    const DVec3 moved =
        among(WorldPosition{eye.metres() + DVec3{box, -3.0 * box, 0.0}}, velocity, 0.0);
    CHECK_THAT(glm::length(moved - at), WithinAbs(0.0, 1e-9));
    // A second on, the eye is as far back among them as they've fallen.
    const DVec3 later = among(eye, velocity, 1.0);
    CHECK_THAT(later.z, WithinAbs(std::fmod(at.z + 7.0, box), 1e-9));
    CHECK_THAT(later.x, WithinAbs(std::fmod(at.x - 1.0 + box, box), 1e-9));
}

TEST_CASE("with nothing falling, nothing is drawn", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    CHECK(lit_pixels(draw(*device, {.falling = 0.0})) == 0);
}

TEST_CASE("rain is drawn round the eye, more of it the heavier", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const std::size_t light = lit_pixels(draw(*device, {.falling = 0.5}));
    const std::size_t heavy = lit_pixels(draw(*device, {.falling = 10.0}));
    INFO("light " << light << ", heavy " << heavy);
    CHECK(light > 0);
    CHECK(heavy > 2 * light);
}

TEST_CASE("rain is hidden by what's drawn nearer", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    CHECK(lit_pixels(draw(*device, {.falling = 10.0, .depth = 1.0})) == 0);
}

TEST_CASE("rain falling round an eye in the sea is gone into it", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    CHECK(lit_pixels(draw(*device, {.falling = 10.0, .height = -1.0})) == 0);
}

TEST_CASE("rain falls as time passes", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    CHECK(draw(*device, {.seconds = 0.0}) != draw(*device, {.seconds = 0.25}));
    // What's seen repeats, as the drops do, once they've fallen a box.
    CHECK(draw(*device, {.seconds = 0.0}) ==
          draw(*device, {.seconds = box / hog::render::rain_speed}));
}

TEST_CASE("snow is drawn as brighter flakes than rain's streaks", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const double rain = mean_lit(draw(*device, {.falling = 2.0}));
    const double snow = mean_lit(draw(*device, {.falling = 2.0, .snow = 1.0}));
    INFO("rain " << rain << ", snow " << snow);
    CHECK(rain > 0.0);
    CHECK(snow > 1.5 * rain);
}
