#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/flora/species.hpp>
#include <hog/render/anchors.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/trees.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::Vec3;
using hog::WorldPosition;
using hog::render::AnchorPool;
using hog::render::GrassView;
using hog::render::StandDraw;
using hog::render::TreeAnchor;
using hog::render::Trees;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 96, .height = 54};
constexpr VkFormat colour_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr double radius = 115'000.0;
constexpr double ground_height = 2.0;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};
constexpr DVec3 north{0.0, 1.0, 0.0};
constexpr Vec3 needles{0.08F, 0.15F, 0.08F};

using Pixels = std::vector<std::uint8_t>;

std::uint32_t hashed(std::uint32_t at) {
    at ^= at >> 16U;
    at *= 0x7feb352dU;
    at ^= at >> 15U;
    at *= 0x846ca68bU;
    at ^= at >> 16U;
    return at;
}

// A rank of pines anchored east to west, `away` metres north of the tile's origin.
std::vector<TreeAnchor> rank_of(int count, double away, float height = 11.0F,
                                float age = hog::flora::prime_age) {
    std::vector<TreeAnchor> anchors;
    for (int tree = 0; tree < count; ++tree) {
        const double spread = (static_cast<double>(tree) - ((count - 1) * 0.5)) * 8.0;
        anchors.push_back({.position = Vec3{spread, away, 0.0},
                           .height = height,
                           .normal = Vec3{0.0F, 0.0F, 1.0F},
                           .hash = hashed(static_cast<std::uint32_t>(tree) * 2654435761U),
                           .colour = hog::render::packed_needles(needles, 1.0F),
                           .age = age});
    }
    return anchors;
}

struct Stand {
    std::vector<TreeAnchor> anchors;
    double hold_off = 0.0;
    DVec3 wind{0.0};
    double seconds = 0.0;
    DVec3 eye_back{0.0, -40.0, 6.0}; // east, north, up of the tile's origin
};

// The stand's stamps alone over black, from south of the origin facing north.
Pixels draw(hog::render::Device& device, const Stand& stand) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    auto trees = Trees::create(device, colour_format, depth_format, stand.hold_off);
    REQUIRE(trees);
    auto pool = AnchorPool::create(device, {.tiles = 1, .frames = 1, .uploads = 1});
    REQUIRE(pool);
    const std::uint32_t slot = pool->acquire().value_or(99);
    REQUIRE(slot == 0);
    REQUIRE(pool->stage(0, slot, stand.anchors));

    const WorldPosition origin{up * (radius + ground_height)};
    const WorldPosition eye{(up * (radius + ground_height + stand.eye_back.z)) +
                            (east * stand.eye_back.x) + (north * stand.eye_back.y)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, ground_height);
    const double elevation = 40.0 * std::numbers::pi / 180.0;
    const DVec3 sun = (east * std::cos(elevation)) + (up * std::sin(elevation));
    const hog::render::Camera camera{.eye = eye, .forward = north, .up = up, .vertical_fov = 0.6};
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), bend);
    const hog::render::SkyView seen{.camera = camera,
                                    .sun = sun,
                                    .bend = bend,
                                    .exposure = 10.0,
                                    .sky_light = model.sky_irradiance(2.0 * scale, sun)};
    const StandDraw stamps{.slot = slot,
                           .count = static_cast<std::uint32_t>(stand.anchors.size()),
                           .origin = origin,
                           .detail = 0};
    const GrassView view{.camera = camera,
                         .bend = bend,
                         .air = sky->bindings(0, *atmosphere),
                         .wind = stand.wind,
                         .seconds = stand.seconds};

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
        trees->draw(commands, extent, *pool, {&stamps, 1}, view);
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

std::size_t barky_pixels(const Pixels& pixels) {
    std::size_t barky = 0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
        const int red = pixels.at(4 * pixel);
        const int green = pixels.at((4 * pixel) + 1);
        const int blue = pixels.at((4 * pixel) + 2);
        barky += (red > 20 && red >= green && red > blue) ? 1U : 0U;
    }
    return barky;
}

// Counted from row `from` down, the picture's top row first.
std::size_t leafy_pixels(const Pixels& pixels, std::uint32_t from = 0) {
    std::size_t leafy = 0;
    for (std::size_t pixel = std::size_t{from} * extent.width; pixel < pixels.size() / 4; ++pixel) {
        const int red = pixels.at(4 * pixel);
        const int green = pixels.at((4 * pixel) + 1);
        leafy += (green > 20 && green > red) ? 1U : 0U;
    }
    return leafy;
}

} // namespace

TEST_CASE("needles pack into a word, dark shades kept", "[render]") {
    const Vec3 dark{0.02F, 0.06F, 0.03F};
    const std::uint32_t packed = hog::render::packed_needles(dark, 0.4F);
    const Vec3 back = hog::render::needles_of(packed);
    for (glm::length_t channel = 0; channel < 3; ++channel) {
        CHECK(std::abs(back[channel] - dark[channel]) < dark[channel] * 0.05F);
    }
    CHECK(std::abs(hog::render::wooded_of(packed) - 0.4F) < 0.003F);
    // Out of range clamps rather than spilling into the next channel.
    const std::uint32_t bright = hog::render::packed_needles(Vec3{2.0F, -1.0F, 0.5F}, 3.0F);
    CHECK(hog::render::needles_of(bright).r == 1.0F);
    CHECK(hog::render::needles_of(bright).g == 0.0F);
    CHECK(hog::render::wooded_of(bright) == 1.0F);
}

TEST_CASE("a stamp stands on its anchor, trunk under a tiered crown", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const Pixels stand = draw(*device, {.anchors = rank_of(5, 0.0)});
    CHECK(lit_pixels(stand) > 150);
    CHECK(barky_pixels(stand) > 2);
    // An empty tile draws nothing.
    CHECK(lit_pixels(draw(*device, {.anchors = {}})) == 0);
}

TEST_CASE("stamps hold off where the grown forest covers, and shrink away far off", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // The same rank 100 m out: held off while the forest's reach covers it, stood without.
    const Stand near{.anchors = rank_of(5, 100.0)};
    CHECK(lit_pixels(draw(*device, near)) > 30);
    const Stand held{.anchors = rank_of(5, 100.0), .hold_off = 240.0};
    CHECK(lit_pixels(draw(*device, held)) == 0);
    // Beyond their own reach, the stamps have shrunk away entirely.
    const Stand gone{.anchors = rank_of(5, 950.0)};
    CHECK(lit_pixels(draw(*device, gone)) == 0);
}

TEST_CASE("the wind rocks the stamps' tops, and still air leaves them still", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const DVec3 wind{14.0, 0.0, 0.0};
    const Stand windy{.anchors = rank_of(5, 0.0), .wind = wind, .seconds = 0.0};
    Stand later = windy;
    later.seconds = 1.1;
    const Pixels one = draw(*device, windy);
    const Pixels two = draw(*device, later);
    CHECK(one != two);
    const Stand still_stand{.anchors = rank_of(5, 0.0)};
    Stand still_later = still_stand;
    still_later.seconds = 1.1;
    CHECK(draw(*device, still_stand) == draw(*device, still_later));
}

TEST_CASE("each stamp stands as its age: saplings green to the ground, old trees bare below",
          "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // The same height at either age, so only the shape differs.
    const Pixels saplings = draw(*device, {.anchors = rank_of(5, 0.0, 11.0F, 0.1F)});
    const Pixels old = draw(*device, {.anchors = rank_of(5, 0.0, 11.0F, 0.97F)});
    CHECK(leafy_pixels(saplings) > 100);
    CHECK(leafy_pixels(old) > 100);
    // The lowest few metres of the trees, above the ground the eye looks down to.
    constexpr std::uint32_t low = 34;
    CHECK(leafy_pixels(saplings, low) > 30);
    CHECK(leafy_pixels(saplings, low) > 2 * leafy_pixels(old, low));
}
