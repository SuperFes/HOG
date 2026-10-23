#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/flora/bushes.hpp>
#include <hog/flora/stand.hpp>
#include <hog/render/anchors.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/bushes.hpp>
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
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::Vec3;
using hog::WorldPosition;
using hog::render::Bushes;
using hog::render::GrassView;
using hog::render::StandDraw;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 96, .height = 54};
constexpr VkFormat colour_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr double radius = 115'000.0;
constexpr double ground_height = 1.0; // above sea level
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};

using Pixels = std::vector<std::uint8_t>;

// A square of bushes of one kind, `spacing` apart, round the ground under the eye.
std::vector<hog::render::BushAnchor>
stand(hog::flora::BushKind kind, Vec3 leaves = Vec3{0.12F, 0.3F, 0.06F}, float spacing = 1.5F) {
    std::vector<hog::render::BushAnchor> bushes;
    constexpr int across = 16;
    for (int i = 0; i < across; ++i) {
        for (int j = 0; j < across; ++j) {
            const auto at = static_cast<std::uint32_t>((i * across) + j);
            bushes.push_back({.position = Vec3{(static_cast<float>(i) - 7.5F) * spacing,
                                               (static_cast<float>(j) - 7.5F) * spacing, 0.0F},
                              .height = 1.0F,
                              .normal = Vec3{0.0F, 0.0F, 1.0F},
                              .hash = hog::flora::cell_hash(at, at * 7U, 0),
                              .colour = hog::render::packed_leaves(leaves, kind),
                              .width = 1.4F});
        }
    }
    return bushes;
}

struct Drawn {
    std::vector<hog::render::BushAnchor> anchors = stand(hog::flora::BushKind::Floofy);
    double fade_far = hog::render::bushes_far;
    DVec3 wind{0.0};
    double seconds = 0.0;
    double above = 24.0; // metres the eye is over the ground
    DVec3 gusts_drifted{0.0};
};

// The bushes alone over black, seen from overhead so every one is in view.
Pixels draw(hog::render::Device& device, const Drawn& drawn) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    auto bushes = Bushes::create(device, colour_format, depth_format, drawn.fade_far);
    REQUIRE(bushes);
    auto pool = hog::render::AnchorPool::create(device, {.tiles = 1, .frames = 1, .uploads = 1});
    REQUIRE(pool);
    const std::uint32_t slot = pool->acquire().value_or(99);
    REQUIRE(slot == 0);
    REQUIRE(pool->stage(0, slot, std::span<const hog::render::BushAnchor>{drawn.anchors}));

    const WorldPosition origin{up * (radius + ground_height)};
    const WorldPosition eye{up * (radius + ground_height + drawn.above)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, ground_height);
    const double elevation = 40.0 * std::numbers::pi / 180.0;
    const DVec3 sun = (east * std::cos(elevation)) + (up * std::sin(elevation));
    // Straight down over the bushes.
    const hog::render::Camera camera{
        .eye = eye, .forward = -up, .up = DVec3{0.0, 1.0, 0.0}, .vertical_fov = 0.9};
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), bend);
    const hog::render::SkyView seen{.camera = camera,
                                    .sun = sun,
                                    .bend = bend,
                                    .exposure = 10.0,
                                    .sky_light = model.sky_irradiance(2.0 * scale, sun),
                                    .gusts_drifted = drawn.gusts_drifted};
    const StandDraw tile{.slot = slot,
                         .first = 0,
                         .count = static_cast<std::uint32_t>(drawn.anchors.size()),
                         .origin = origin};
    const GrassView view{.camera = camera,
                         .bend = bend,
                         .air = sky->bindings(0, *atmosphere),
                         .wind = drawn.wind,
                         .seconds = drawn.seconds};

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
        CHECK(pool->record_uploads(commands, 0));
        CHECK(sky->record_frame(commands, extent, 0, *atmosphere, seen));
        hog::render::transition(commands, colour->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, colour->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view(), .clear = 0.0F});
        bushes->draw(commands, extent, *pool, {&tile, 1}, view);
        vkCmdEndRendering(commands);
    }));
    CHECK(device.validation_errors() == 0);
    auto pixels = hog::render::read_pixels(device, *colour, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

std::size_t lit_pixels(const Pixels& pixels) {
    std::size_t lit = 0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
        const int brightness =
            pixels.at(4 * pixel) + pixels.at((4 * pixel) + 1) + pixels.at((4 * pixel) + 2);
        lit += brightness > 0 ? 1U : 0U;
    }
    return lit;
}

// Summed over the picture, red then green.
std::array<std::size_t, 2> red_green(const Pixels& pixels) {
    std::array<std::size_t, 2> sums{};
    for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
        sums[0] += pixels.at(4 * pixel);
        sums[1] += pixels.at((4 * pixel) + 1);
    }
    return sums;
}

} // namespace

TEST_CASE("bushes stand where they are anchored, of every kind", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::flora::BushKind;
    const Pixels floofy = draw(*device, {});
    CHECK(lit_pixels(floofy) > 200);
    CHECK(lit_pixels(draw(*device, {.anchors = {}})) == 0);
    const Pixels spiky = draw(*device, {.anchors = stand(BushKind::Spiky)});
    const Pixels twiggy = draw(*device, {.anchors = stand(BushKind::Twiggy)});
    const Pixels mats = draw(*device, {.anchors = stand(BushKind::Mat)});
    CHECK(lit_pixels(spiky) > 0);
    CHECK(lit_pixels(mats) > 0);
    // A twiggy skeleton is mostly air.
    CHECK(lit_pixels(twiggy) > 0);
    CHECK(lit_pixels(twiggy) < lit_pixels(floofy) / 2);
}

TEST_CASE("a bush's leaves take its anchor's colour", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::flora::BushKind;
    const auto green = red_green(draw(*device, {}));
    const auto red =
        red_green(draw(*device, {.anchors = stand(BushKind::Floofy, Vec3{0.4F, 0.05F, 0.03F})}));
    CHECK(green[1] > green[0]);
    CHECK(red[0] > red[1]);
}

TEST_CASE("bushes shrink away at the edge of their reach", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // The bushes lie some 24 to 28 metres from the eye overhead.
    const std::size_t near = lit_pixels(draw(*device, {.fade_far = 200.0}));
    const std::size_t shrinking = lit_pixels(draw(*device, {.fade_far = 31.0}));
    CHECK(near > 200);
    CHECK(shrinking > 0);
    CHECK(shrinking < near);
    CHECK(lit_pixels(draw(*device, {.fade_far = 20.0})) == 0);
}

TEST_CASE("the wind rustles the bushes, and still air leaves them still", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const DVec3 wind = east * 9.0;
    const Pixels now = draw(*device, {.wind = wind, .seconds = 1.0});
    const Pixels later = draw(*device, {.wind = wind, .seconds = 1.7});
    CHECK(now != later);
    const Pixels still_now = draw(*device, {.seconds = 1.0});
    const Pixels still_later = draw(*device, {.seconds = 1.7});
    CHECK(still_now == still_later);
}

TEST_CASE("gusts move the bushes as they drift over them", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const DVec3 wind = east * 9.0;
    const DVec3 drifted = east * 30.0;
    CHECK(draw(*device, {.wind = wind, .seconds = 1.0}) !=
          draw(*device, {.wind = wind, .seconds = 1.0, .gusts_drifted = drifted}));
    CHECK(draw(*device, {.seconds = 1.0}) ==
          draw(*device, {.seconds = 1.0, .gusts_drifted = drifted}));
}

TEST_CASE("a bush's leaves pack with its kind, as a tree's foliage packs", "[render]") {
    using hog::flora::BushKind;
    const Vec3 leaves{0.12F, 0.3F, 0.06F};
    for (const BushKind kind :
         {BushKind::Floofy, BushKind::Spiky, BushKind::Twiggy, BushKind::Mat}) {
        const std::uint32_t packed = hog::render::packed_leaves(leaves, kind);
        CHECK(hog::render::kind_of(packed) == kind);
        CHECK(hog::render::foliage_of(packed).g ==
              hog::render::foliage_of(hog::render::packed_foliage(leaves, hog::flora::Genus::Pine))
                  .g);
    }
}

TEST_CASE("round bushes seen from overhead cover their footprint, not a cross", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::flora::BushKind;
    constexpr std::size_t picture = std::size_t{extent.width} * extent.height;
    // Near enough that each bush is shaped in full, the stand filling the picture, and far enough
    // that each is a tuft, the stand filling half of it.
    for (const auto& [above, least] :
         {std::pair{5.0, picture * 2 / 5}, std::pair{24.0, picture / 4}}) {
        for (const BushKind kind : {BushKind::Floofy, BushKind::Mat}) {
            CAPTURE(above, kind);
            CHECK(lit_pixels(draw(*device, {.anchors = stand(kind), .above = above})) > least);
        }
    }
}
