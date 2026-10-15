#include "test_device.hpp"

#include <hog/core/math.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/terrain.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/material.hpp>
#include <hog/terrain/stitching.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::planet::Biome;
using hog::render::OceanView;
using hog::render::testing::test_device;
using hog::terrain::patch_cells;
using hog::terrain::patch_side;
using hog::terrain::PatchVertex;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

constexpr VkExtent2D extent{.width = 64, .height = 64};
constexpr Pixel background{0, 0, 0, 255};
constexpr double radius = 115'000.0;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};
constexpr DVec3 north{0.0, 1.0, 0.0};

// `height` metres above sea level, over the top of the planet.
OceanView seen_from(double height, DVec3 forward, DVec3 camera_up, DVec3 sun) {
    const WorldPosition eye{up * (radius + height)};
    return {.camera = {.eye = eye, .forward = forward, .up = camera_up},
            .sun = sun,
            .sky = {0.2F, 0.4F, 0.8F},
            .bend = hog::terrain::bend_from(eye, radius, 0.0)};
}

// Level ground two metres across, facing up.
std::vector<PatchVertex> flat_patch() {
    constexpr float spacing = 2.0F / patch_cells;
    constexpr int middle = patch_cells / 2;
    std::vector<PatchVertex> vertices;
    for (int j = 0; j < patch_side; ++j) {
        for (int i = 0; i < patch_side; ++i) {
            vertices.push_back(
                {.position = hog::Vec3{static_cast<float>(i - middle) * spacing,
                                       static_cast<float>(j - middle) * spacing, 0.0F},
                 .normal = {0.0F, 0.0F, 1.0F},
                 .biome = Biome::Grassland});
        }
    }
    for (int edge = 0; edge < 4; ++edge) {
        for (int along = 0; along < patch_side; ++along) {
            const hog::terrain::GridPoint above = hog::terrain::on_edge(edge, along);
            vertices.push_back(vertices.at((static_cast<std::size_t>(above.j) * patch_side) +
                                           static_cast<std::size_t>(above.i)));
        }
    }
    return vertices;
}

// Draws over a black background the ground, if any is `ground` metres above sea level under the
// eye, tinted below sea level if asked, and then, if asked, the sea's surface; reads it back.
std::vector<std::uint8_t> draw(hog::render::Device& device, const OceanView& view,
                               std::optional<double> ground, bool sea = true, bool tinted = false) {
    auto ocean = hog::render::Ocean::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(ocean);
    std::vector<hog::terrain::MaterialImage> materials;
    for (int biome = 0; biome < hog::planet::biome_count; ++biome) {
        const DVec3 colour =
            static_cast<Biome>(biome) == Biome::Grassland ? DVec3{0.0, 0.75, 0.25} : DVec3{0.0};
        materials.push_back(hog::terrain::material_image({.colour = colour}, 64));
    }
    auto terrain =
        hog::render::Terrain::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                                     {.materials = materials, .sea = tinted});
    REQUIRE(terrain);
    auto pool = hog::render::TerrainPool::create(device, {});
    REQUIRE(pool);
    std::vector<hog::render::TerrainDraw> draws;
    if (ground) {
        const std::uint32_t slot = pool->acquire().value_or(1);
        REQUIRE(pool->stage(0, slot, flat_patch()));
        draws.push_back({.slot = slot, .origin = WorldPosition{up * (radius + *ground)}});
    }

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
        REQUIRE(pool->record_uploads(commands, 0));
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, target->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        terrain->draw(commands, extent, *pool, draws,
                      {.camera = view.camera, .sun = view.sun, .bend = view.bend});
        if (sea) {
            ocean->draw(commands, extent, view);
        }
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

TEST_CASE("over open sea, the sea is drawn out to the horizon", "[render][gpu]") {
    const auto device = test_device();
    // Walking height looking level, along the line of the horizon a pixel's middle off it.
    const auto walking = draw(*device, seen_from(3.0, east, up, north), std::nullopt);
    for (std::uint32_t x = 0; x < extent.width; ++x) {
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            INFO("at " << x << ", " << y);
            CHECK((pixel_at(walking, x, y) == background) == (y < extent.height / 2));
        }
    }

    // From orbit, the whole of the side seen, about 13 pixels round.
    const auto orbit = draw(*device, seen_from(400'000.0, -up, north, east), std::nullopt);
    const std::uint32_t middle = extent.width / 2;
    CHECK(pixel_at(orbit, middle, middle) != background);
    CHECK(pixel_at(orbit, middle + 10, middle) != background);
    CHECK(pixel_at(orbit, middle, middle - 10) != background);
    CHECK(pixel_at(orbit, middle + 17, middle) == background);
    CHECK(pixel_at(orbit, 0, 0) == background);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("ground above sea level hides the sea, and the sea lies over ground below it",
          "[render][gpu]") {
    const auto device = test_device();
    // Looking down with the sun overhead, at its glint.
    const OceanView view = seen_from(4.0, -up, north, up);
    const std::uint32_t middle = extent.width / 2;

    const Pixel dry = pixel_at(draw(*device, view, 1.0, false), middle, middle);
    CHECK(pixel_at(draw(*device, view, 1.0), middle, middle) == dry);

    const Pixel drowned = pixel_at(draw(*device, view, -1.0, false), middle, middle);
    const Pixel under = pixel_at(draw(*device, view, -1.0), middle, middle);
    CHECK(drowned == dry);
    CHECK(under[0] > drowned[0] + 100);

    // Away from the glint, looking straight down, the ground beneath shows through.
    const OceanView aside = seen_from(4.0, -up, north, glm::normalize(east + up));
    const Pixel beneath = pixel_at(draw(*device, aside, -1.0, false), middle, middle);
    const Pixel through = pixel_at(draw(*device, aside, -1.0), middle, middle);
    CHECK(through[1] + 10 > beneath[1]);
    CHECK(through[1] < beneath[1]);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the sea reflects more of the sky the more glancing the view", "[render][gpu]") {
    const auto device = test_device();
    // The sun along the horizon behind, out of the way.
    const auto level = draw(*device, seen_from(3.0, east, up, -east), std::nullopt);
    const std::uint32_t middle = extent.width / 2;
    const Pixel near_horizon = pixel_at(level, middle, (extent.height / 2) + 1);
    const Pixel underfoot = pixel_at(level, middle, extent.height - 1);
    CHECK(near_horizon[2] > underfoot[2] + 50);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("ground below sea level is lost in the water's colour the deeper it lies",
          "[render][gpu]") {
    const auto device = test_device();
    const OceanView view = seen_from(4.0, -up, north, glm::normalize(east + up));
    const std::uint32_t middle = extent.width / 2;
    const auto seen = [&](double ground, bool tinted) {
        return pixel_at(draw(*device, view, ground, false, tinted), middle, middle);
    };

    CHECK(seen(1.0, true) == seen(1.0, false));
    const Pixel shallow = seen(-1.0, true);
    const Pixel deep = seen(-20.0, true);
    CHECK(shallow[1] < seen(-1.0, false)[1]);
    CHECK(shallow[1] > deep[1] + 50);
    // Most of the way to the water's own colour, lit as the ground is.
    CHECK(deep[1] < 40);
    CHECK(device->validation_errors() == 0);
}
