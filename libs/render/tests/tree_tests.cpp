#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/planet/climate.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/trees.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/patch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::Vec3;
using hog::WorldPosition;
using hog::render::GrassView;
using hog::render::TreeDraw;
using hog::render::Trees;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 96, .height = 54};
constexpr VkFormat colour_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr double radius = 115'000.0;
constexpr double ground_height = 2.0; // above sea level, clear of the trees' shore
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};
constexpr DVec3 north{0.0, 1.0, 0.0};

using Pixels = std::vector<std::uint8_t>;

// A flat patch of one biome, level at `ground_height`, centred on its origin.
std::vector<hog::terrain::PatchVertex> field(hog::planet::Biome biome, double spacing) {
    std::vector<hog::terrain::PatchVertex> vertices(hog::render::patch_vertex_count);
    constexpr int middle = hog::terrain::patch_cells / 2;
    for (int i = 0; i < hog::terrain::patch_side; ++i) {
        for (int j = 0; j < hog::terrain::patch_side; ++j) {
            const std::size_t at = (static_cast<std::size_t>(i) * hog::terrain::patch_side) +
                                   static_cast<std::size_t>(j);
            vertices[at] = {.position = Vec3{static_cast<float>((i - middle) * spacing),
                                             static_cast<float>((j - middle) * spacing), 0.0F},
                            .normal = Vec3{0.0F, 0.0F, 1.0F},
                            .biome = biome};
        }
    }
    return vertices;
}

struct Patch {
    hog::planet::Biome biome = hog::planet::Biome::BorealForest;
    double spacing = 4.0;
    DVec3 offset{0.0}; // of the patch's origin, metres east and north of the up point
    std::uint32_t lattice_i = 4;
    std::uint32_t lattice_j = 4;
    int shift = 0;
    DVec3 wind{0.0};
    double seconds = 0.0;
    bool from_side = false; // low from the south, close enough to see the trunks
};

// The patch's trees alone over black: from overhead so every one is in view, or from the side.
Pixels draw(hog::render::Device& device, const Patch& grown) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    std::array<Vec3, hog::planet::biome_count> palette{};
    palette.fill(Vec3{0.35F, 0.55F, 0.25F});
    auto trees = Trees::create(device, colour_format, depth_format, palette);
    REQUIRE(trees);
    auto pool = hog::render::TerrainPool::create(device, {.patches = 1, .frames = 1, .uploads = 1});
    REQUIRE(pool);
    const std::uint32_t slot = pool->acquire().value_or(99);
    REQUIRE(slot == 0);
    REQUIRE(pool->stage(0, slot, field(grown.biome, grown.spacing)));

    const WorldPosition origin{(up * (radius + ground_height)) + (east * grown.offset.x) +
                               (north * grown.offset.y)};
    const WorldPosition eye{grown.from_side
                                ? (up * (radius + ground_height + 9.0)) + (north * -30.0)
                                : up * (radius + ground_height + 80.0)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, ground_height);
    const double elevation = 40.0 * std::numbers::pi / 180.0;
    const DVec3 sun = (east * std::cos(elevation)) + (up * std::sin(elevation));
    const hog::render::Camera camera{.eye = eye,
                                     .forward = grown.from_side ? north : -up,
                                     .up = grown.from_side ? up : north,
                                     .vertical_fov = 0.6};
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), bend);
    const hog::render::SkyView seen{.camera = camera,
                                    .sun = sun,
                                    .bend = bend,
                                    .exposure = 10.0,
                                    .sky_light = model.sky_irradiance(2.0 * scale, sun)};
    const TreeDraw stand{.slot = slot,
                         .origin = origin,
                         .face = 0,
                         .lattice_i = grown.lattice_i,
                         .lattice_j = grown.lattice_j,
                         .shift = grown.shift,
                         .fade_far = 900.0};
    const GrassView view{.camera = camera,
                         .bend = bend,
                         .air = sky->bindings(0, *atmosphere),
                         .wind = grown.wind,
                         .seconds = grown.seconds};

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
        trees->draw(commands, extent, *pool, {&stand, 1}, view);
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
        barky += (red > 20 && red > green && red > blue) ? 1U : 0U;
    }
    return barky;
}

} // namespace

TEST_CASE("pines grow where their biome grows them", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const std::size_t boreal =
        lit_pixels(draw(*device, {.biome = hog::planet::Biome::BorealForest}));
    CHECK(boreal > 100);
    CHECK(lit_pixels(draw(*device, {.biome = hog::planet::Biome::Ocean})) == 0);
    // A grassland grows the odd lone pine, far sparser than a forest.
    const std::size_t lone = lit_pixels(draw(*device, {.biome = hog::planet::Biome::Grassland}));
    CHECK(lone < boreal / 3);
}

TEST_CASE("a pine stands its ground at every terrain level", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // The same ground baked fine and coarse: the finer patch spans the south-west quarter of the
    // coarser, so it shares its lattice base, keyed a shift apart; the finer grid packs more
    // cells into each lattice cell. The same pines must stand in the same spots either way.
    const Patch coarse{.spacing = 8.0, .offset = DVec3{64.0, 64.0, 0.0}, .shift = 0};
    const Patch fine{.spacing = 4.0, .offset = DVec3{0.0, 0.0, 0.0}, .shift = 1};
    const Pixels coarsely = draw(*device, coarse);
    const Pixels finely = draw(*device, fine);
    CHECK(lit_pixels(finely) > 20);
    CHECK(coarsely == finely);
}

TEST_CASE("pines carry bare trunks under their crowns", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const Pixels stand = draw(*device, {.from_side = true});
    CHECK(lit_pixels(stand) > 200);
    CHECK(barky_pixels(stand) > 5);
}

TEST_CASE("the wind rocks the pines' tops, and still air leaves them still", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const DVec3 wind = east * 9.0;
    const Pixels now = draw(*device, {.wind = wind, .seconds = 1.0, .from_side = true});
    const Pixels later = draw(*device, {.wind = wind, .seconds = 1.7, .from_side = true});
    CHECK(now != later);
    const Pixels still_now = draw(*device, {.seconds = 1.0, .from_side = true});
    const Pixels still_later = draw(*device, {.seconds = 1.7, .from_side = true});
    CHECK(still_now == still_later);
}
