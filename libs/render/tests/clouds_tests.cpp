#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/clouds.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/weather_maps.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>

#include <algorithm>
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
// Overcast, raining about 2.7 mm and 26 mm an hour, and the first as snow.
const Texel raining{255, 64, 26, 0};
const Texel pouring{255, 64, 80, 0};
const Texel snowing{255, 64, 26, 255};
// As client::SkyTexel: dry, with cloud from 1984 m to 3008 m.
const Texel layer{0, 0, 31, 47};

struct Scene {
    double eye_height = 2.0;
    DVec3 forward = east;
    double sun_elevation = 60.0; // degrees, towards the east
    std::vector<std::byte> before = everywhere(overcast);
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::optional<std::vector<std::byte>> after = {};
    double faded = 1.0;
    std::vector<std::byte> sky = everywhere(layer);
    DVec3 moved{0.0};
    // Drawn nearest of all.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::optional<VkRect2D> covered = {};
};

// The clouds alone, over black, for each of `scenes` in turn as frames of one picture.
std::vector<Pixels> draw_frames(hog::render::Device& device, const std::vector<Scene>& scenes) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    hog::jobs::JobSystem jobs{2};
    auto clouds = Clouds::create(device, colour_format, jobs);
    REQUIRE(clouds);
    auto maps = hog::render::WeatherMaps::create(device, map_size);
    REQUIRE(maps);
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
    const auto march_image = [&](VkFormat format) {
        auto image = hog::render::make_image(
            device, {.format = format,
                     .extent = Clouds::march_extent(extent),
                     .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
        REQUIRE(image);
        return std::move(image).value();
    };
    const hog::render::Image marched = march_image(Clouds::marched_format);
    const hog::render::Image distances = march_image(Clouds::distances_format);

    std::vector<Pixels> frames;
    for (const Scene& scene : scenes) {
        CHECK(maps->map_bytes() == scene.before.size());
        REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
            CHECK(maps->update(commands, 0, scene.before, scene.after.value_or(scene.before),
                               scene.sky, scene.sky));
        }));
        const hog::render::WeatherBindings weather = maps->bindings(scene.faded);

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
                                            model.sky_irradiance(scene.eye_height * scale, sun),
                                        .weather = weather};
        const CloudsView view{.camera = camera,
                              .bend = bend,
                              .air = sky->bindings(0, *atmosphere),
                              .base = 1'984.0,
                              .weather = weather,
                              .moved = scene.moved};
        REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
            namespace use = hog::render::image_use;
            CHECK(sky->record_frame(commands, extent, 0, *atmosphere, seen));
            hog::render::transition(commands, colour->handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                    use::discard, use::colour_target);
            hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                                    use::discard, use::depth_target);
            hog::render::begin_rendering(commands, colour->view(), extent,
                                         VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                         {.view = depth->view()});
            if (scene.covered) {
                const VkClearAttachment nearest{
                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .colorAttachment = 0,
                    .clearValue = {.depthStencil = {.depth = 1.0F, .stencil = 0}}};
                const VkClearRect rect{
                    .rect = *scene.covered, .baseArrayLayer = 0, .layerCount = 1};
                vkCmdClearAttachments(commands, 1, &nearest, 1, &rect);
            }
            vkCmdEndRendering(commands);
            hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
                                    use::depth_target, use::sampled);
            for (const hog::render::Image* image : {&marched, &distances}) {
                hog::render::transition(commands, image->handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                        use::discard, use::storage);
            }
            CHECK(clouds->march(commands, extent, depth->view(), marched.view(), distances.view(),
                                view));
            hog::render::transition(commands, distances.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                    use::storage, use::sampled);
            hog::render::begin_rendering(commands, colour->view(), extent, std::nullopt);
            clouds->draw(commands, extent, depth->view(), distances.view(), view);
            vkCmdEndRendering(commands);
        }));
        CHECK(device.validation_errors() == 0);
        auto pixels =
            hog::render::read_pixels(device, *colour, hog::render::image_use::colour_target);
        REQUIRE(pixels);
        frames.push_back(std::move(pixels).value());
    }
    return frames;
}

// The clouds alone, over black.
Pixels draw(hog::render::Device& device, const Scene& scene) {
    return draw_frames(device, {scene}).front();
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

// Of the rows below the horizon, when looking level.
std::uint32_t lit_below(const Pixels& pixels) {
    std::uint32_t lit = 0;
    for (std::uint32_t y = (extent.height / 2) + 1; y < extent.height; ++y) {
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

TEST_CASE("cloud shades the ground the more of it there is", "[render]") {
    using hog::render::cloud_shade;
    const hog::render::CloudShade open_sky = cloud_shade(0.0, 0.0);
    CHECK(open_sky.direct == 1.0);
    CHECK(open_sky.diffuse == 0.0);
    // Half covered by thick cloud, half the sun gets through, and a little more scattered.
    const hog::render::CloudShade gappy = cloud_shade(0.5, 1.0);
    CHECK(std::abs(gappy.direct - 0.5) < 1e-6);
    CHECK(gappy.diffuse > 0.0);
    const hog::render::CloudShade covered = cloud_shade(1.0, 1.0);
    CHECK(covered.direct < 1e-6);
    CHECK(covered.diffuse > 0.02);
    const hog::render::CloudShade heavy = cloud_shade(1.0, 8.0);
    CHECK(heavy.diffuse < covered.diffuse);
    // A wisp lets most through, scattering some on the way.
    const hog::render::CloudShade wisp = cloud_shade(1.0, 0.001);
    CHECK(wisp.direct > 0.8);
    CHECK(wisp.direct + wisp.diffuse <= 1.0);
}

TEST_CASE("mist takes the sun's light and scatters most of it on down", "[render]") {
    using hog::render::under_mist;
    const hog::render::CloudShade sunny{.direct = 1.0, .diffuse = 0.0};
    const hog::render::CloudShade dry = under_mist(sunny, 0.0, 0.0, 10.0, 0.5);
    CHECK(dry.direct == 1.0);
    CHECK(dry.diffuse == 0.0);
    const hog::render::CloudShade hazy = under_mist(sunny, 0.9, 0.0, 10.0, 0.5);
    CHECK(hazy.direct < 1.0);
    CHECK(hazy.diffuse > 0.0);
    const hog::render::CloudShade lower = under_mist(sunny, 0.9, 0.0, 10.0, 0.2);
    CHECK(lower.direct < hazy.direct);
    const hog::render::CloudShade fogged = under_mist(sunny, 0.5, 300.0, 10.0, 0.5);
    CHECK(fogged.direct < 0.01);
    CHECK(fogged.diffuse > 0.6);
    CHECK(fogged.direct + fogged.diffuse < 1.0);
}

TEST_CASE("rain hides more the heavier it falls, and snow more than rain", "[render]") {
    using hog::render::rain_extinction;
    CHECK(rain_extinction(0.0, 0.0) == 0.0);
    CHECK(rain_extinction(-1.0, 1.0) == 0.0);
    // Moderate rain is seen through for some kilometres, snow for one or so.
    CHECK(rain_extinction(2.5, 0.0) * 5000.0 > 1.0);
    CHECK(rain_extinction(2.5, 0.0) * 5000.0 < 5.0);
    CHECK(rain_extinction(25.0, 0.0) > 2.0 * rain_extinction(2.5, 0.0));
    CHECK(rain_extinction(1.0, 1.0) * 1000.0 > 2.0);
    CHECK(rain_extinction(1.0, 0.5) < rain_extinction(1.0, 1.0));
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
    // Looking well up, through the layer, so gaps show.
    const Scene fading{.forward = glm::normalize(east + (2.0 * up)),
                       .before = everywhere(clear),
                       .after = everywhere(overcast)};
    Scene start = fading;
    start.faded = 0.0;
    Scene halfway = fading;
    halfway.faded = 0.5;
    const std::uint32_t none = lit_pixels(draw(*device, start), extent.height);
    const std::uint32_t some = lit_pixels(draw(*device, halfway), extent.height);
    const std::uint32_t all = lit_pixels(draw(*device, fading), extent.height);
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
    // Looking well up, through the layer, so gaps show.
    const Scene still{.forward = glm::normalize(east + (2.0 * up)), .before = everywhere(broken)};
    Scene around = still;
    around.moved = DVec3{cloud_period, -2.0 * cloud_period, 0.0};
    Scene drifted = still;
    drifted.moved = DVec3{3000.0, 0.0, 0.0};
    const Pixels pixels = draw(*device, still);
    CHECK(lit_pixels(pixels, extent.height) > 0);
    CHECK(lit_pixels(pixels, extent.height) < extent.width * extent.height);
    CHECK(draw(*device, around) == pixels);
    CHECK(draw(*device, drifted) != pixels);
}

TEST_CASE("thick cloud is dark beneath and bright on top", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Pixels beneath = draw(*device, {.forward = glm::normalize(east + up)});
    const Pixels on_top =
        draw(*device, {.eye_height = 4000.0, .forward = glm::normalize(east - up)});
    const long under = total(beneath, extent.height);
    const long over = total(on_top, extent.height);
    INFO("beneath " << under << ", on top " << over);
    CHECK(under > 0);
    CHECK(2 * over > 3 * under);
}

TEST_CASE("cloud beyond the ground isn't drawn", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    CHECK(lit_below(draw(*device, {})) == 0);
}

TEST_CASE("rain far off hangs beneath the cloud, thicker the heavier it falls",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    // Under no cloud, seen to the horizon, kilometres off.
    const Pixels alone = draw(*device, {.before = everywhere({0, 0, 26, 0})});
    CHECK(lit_pixels(alone) == extent.width * (extent.height / 2));
    // Under cloud, between it and the eye, and duller than its underside.
    const Pixels dry = draw(*device, {});
    const long light = total(draw(*device, {.before = everywhere(raining)}));
    const long heavy = total(draw(*device, {.before = everywhere(pouring)}));
    INFO("dry " << total(dry) << ", light " << light << ", heavy " << heavy);
    CHECK(light < total(dry));
    CHECK(heavy < light);
}

TEST_CASE("snow hangs paler than rain", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const long rain = total(draw(*device, {.before = everywhere(raining)}), extent.height);
    const long snow = total(draw(*device, {.before = everywhere(snowing)}), extent.height);
    INFO("rain " << rain << ", snow " << snow);
    CHECK(snow > rain);
}

TEST_CASE("rain beneath is seen from above through gaps in the cloud",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Scene above{.eye_height = 4000.0,
                      .forward = glm::normalize(east - up),
                      .before = everywhere({0, 0, 26, 0})};
    CHECK(lit_pixels(draw(*device, above), extent.height) == extent.width * extent.height);
    Scene dry = above;
    dry.before = everywhere(clear);
    CHECK(lit_pixels(draw(*device, dry), extent.height) == 0);
}

TEST_CASE("rain is hidden by what's drawn in front of it", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const VkRect2D covered{.offset = {.x = 10, .y = 2}, .extent = {.width = 8, .height = 6}};
    const Pixels pixels = draw(*device, {.before = everywhere({0, 0, 26, 0}), .covered = covered});
    CHECK(brightness(pixels, 12, 4) == 0);
    CHECK(brightness(pixels, 4, 4) > 0);
}

namespace {

// Saturated under fog to 288 m, as client::SkyTexel holds it.
const Texel foggy{255, 9, 0, 0};

} // namespace

TEST_CASE("fog hides all round the eye but what's drawn nearest, lit by the day",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const VkRect2D covered{.offset = {.x = 10, .y = 2}, .extent = {.width = 8, .height = 6}};
    const Pixels pixels =
        draw(*device, {.before = everywhere(clear), .sky = everywhere(foggy), .covered = covered});
    CHECK(brightness(pixels, 12, 4) == 0);
    // Above the horizon: below it the sea's near, and seen through less of the fog.
    std::uint32_t grey = 0;
    int dimmest = 765;
    for (std::uint32_t y = 0; y < extent.height / 2; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            if (x >= 10 && x < 18 && y >= 2 && y < 8) {
                continue;
            }
            dimmest = std::min(dimmest, brightness(pixels, x, y));
            grey += brightness(pixels, x, y) > 300 ? 1U : 0U;
        }
    }
    INFO("dimmest " << dimmest);
    CHECK(grey == (extent.width * (extent.height / 2)) - (8 * 6));
}

TEST_CASE("from over fog, its top lies below and the air over it is clear",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Scene over{.eye_height = 1'000.0,
                     .forward = glm::normalize(east - (up * 0.3)),
                     .before = everywhere(clear),
                     .sky = everywhere(foggy)};
    const Pixels pixels = draw(*device, over);
    // Looking down below the horizon, at the fog; and up, at nothing.
    CHECK(brightness(pixels, extent.width / 2, extent.height - 1) > 300);
    CHECK(brightness(pixels, extent.width / 2, 0) == 0);
    Scene dry = over;
    dry.sky = everywhere(clear);
    CHECK(lit_pixels(draw(*device, dry), extent.height) == 0);
}

TEST_CASE("flying through cloud, it's grey all round", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Pixels pixels = draw(*device, {.eye_height = 2'500.0});
    CHECK(lit_pixels(pixels, extent.height) == extent.width * extent.height);
    // Grey: as red nearly as it's blue.
    const std::size_t middle =
        4 * ((std::size_t{extent.height / 2} * extent.width) + (extent.width / 2));
    INFO(int{pixels[middle]} << ", " << int{pixels[middle + 1]} << ", " << int{pixels[middle + 2]});
    CHECK(pixels[middle + 2] < pixels[middle] + (pixels[middle] / 2) + 4);
}

TEST_CASE("clouds lie between the heights the sky has them at", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    // From 1000 m to 1536 m, seen from over it and from under it.
    const auto low = everywhere({0, 0, 16, 24});
    const Pixels over =
        draw(*device, {.eye_height = 2'000.0, .before = everywhere(overcast), .sky = low});
    CHECK(lit_pixels(over) == 0);
    CHECK(lit_below(over) > 0);
    const Pixels under =
        draw(*device, {.eye_height = 500.0, .before = everywhere(overcast), .sky = low});
    CHECK(lit_pixels(under) > 0);
}

TEST_CASE("a storm towers over the thin cloud round it", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    // Cloud to 3008 m, but to 9984 m in the north.
    const auto towering =
        map_of([](DVec3 direction) { return direction.y > 0.2 ? Texel{0, 0, 31, 156} : layer; });
    const Pixels north_view = draw(
        *device,
        {.eye_height = 5'000.0, .forward = north, .before = everywhere(overcast), .sky = towering});
    const Pixels south_view = draw(*device, {.eye_height = 5'000.0,
                                             .forward = -north,
                                             .before = everywhere(overcast),
                                             .sky = towering});
    CHECK(lit_pixels(north_view) > 0);
    CHECK(lit_pixels(south_view) == 0);
}

namespace {

long difference(const Pixels& left, const Pixels& right) {
    long sum = 0;
    for (std::size_t at = 0; at < left.size(); ++at) {
        sum += std::abs(int{left[at]} - int{right[at]});
    }
    return sum;
}

// Broken cloud overhead, looking well up through it.
Scene broken_overhead() {
    return {.forward = glm::normalize(east + (2.0 * up)), .before = everywhere(broken)};
}

} // namespace

TEST_CASE("frames blend with those before, steadying while the eye's still",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const std::vector<Pixels> frames =
        draw_frames(*device, std::vector<Scene>(10, broken_overhead()));
    const long first = difference(frames[0], frames[1]);
    const long last = difference(frames[8], frames[9]);
    INFO("first " << first << ", last " << last);
    // Each frame's march is offset afresh, so they differ, less as more are blended.
    CHECK(first > 0);
    CHECK(last < first);
}

TEST_CASE("when the eye jumps, what was marched before is dropped", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Scene still = broken_overhead();
    Scene jumped = still;
    jumped.eye_height = 1'500.0;
    const std::vector<Pixels> frames = draw_frames(*device, {still, still, still, jumped});
    CHECK(frames.back() == draw(*device, jumped));
}

TEST_CASE("turning, what was marched before follows the view", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const Scene still = broken_overhead();
    Scene turned = still;
    turned.forward = glm::normalize(east + (0.1 * north) + (2.0 * up));
    std::vector<Scene> turning(6, still);
    turning.push_back(turned);
    const Pixels after_turning = draw_frames(*device, turning).back();
    const Pixels settled = draw_frames(*device, std::vector<Scene>(7, turned)).back();
    const Pixels unturned = draw_frames(*device, std::vector<Scene>(7, still)).back();
    INFO("after turning " << difference(after_turning, settled) << ", unturned "
                          << difference(unturned, settled));
    CHECK(difference(after_turning, settled) < difference(unturned, settled) / 2);
}
