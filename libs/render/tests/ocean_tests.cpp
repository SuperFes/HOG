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
#include <cmath>
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

// A patch's vertices, the grid's from `grid_vertex(i, j)` followed by its edges'.
template <typename GridVertex>
std::vector<PatchVertex> patch_of(GridVertex grid_vertex) {
    std::vector<PatchVertex> vertices;
    for (int j = 0; j < patch_side; ++j) {
        for (int i = 0; i < patch_side; ++i) {
            vertices.push_back(grid_vertex(i, j));
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

// Level ground two metres across, facing up.
std::vector<PatchVertex> flat_patch() {
    constexpr float spacing = 2.0F / patch_cells;
    constexpr int middle = patch_cells / 2;
    return patch_of([](int i, int j) {
        return PatchVertex{.position = hog::Vec3{static_cast<float>(i - middle) * spacing,
                                                 static_cast<float>(j - middle) * spacing, 0.0F},
                           .normal = {0.0F, 0.0F, 1.0F},
                           .biome = Biome::Grassland};
    });
}

struct Patch {
    WorldPosition origin;
    std::vector<PatchVertex> vertices;
};

// Ground `height` metres above sea level all over, `across` metres square, its middle `along`
// metres east over the ground from the top of the planet.
Patch level_patch(double along, double across, double height) {
    const double turned = along / radius;
    const DVec3 middle = (up * std::cos(turned)) + (east * std::sin(turned));
    const DVec3 eastward = (east * std::cos(turned)) - (up * std::sin(turned));
    const DVec3 origin = middle * (radius + height);
    const double spacing = across / patch_cells;
    constexpr int half = patch_cells / 2;
    return {.origin = WorldPosition{origin},
            .vertices = patch_of([&](int i, int j) {
                const DVec3 out = glm::normalize(
                    (middle * radius) + (eastward * (static_cast<double>(i - half) * spacing)) +
                    (north * (static_cast<double>(j - half) * spacing)));
                return PatchVertex{.position = hog::Vec3{(out * (radius + height)) - origin},
                                   .normal = hog::Vec3{out},
                                   .biome = Biome::Grassland};
            })};
}

// Draws over a black background the ground, if any, tinted below sea level if asked, and then, if
// asked, the sea's surface; reads it back.
std::vector<std::uint8_t> draw_patch(hog::render::Device& device, const OceanView& view,
                                     const std::optional<Patch>& ground, bool sea, bool tinted) {
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
        REQUIRE(pool->stage(0, slot, ground->vertices));
        draws.push_back({.slot = slot, .origin = ground->origin});
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

// As draw_patch, with level ground, if any, `ground` metres above sea level under the eye.
std::vector<std::uint8_t> draw(hog::render::Device& device, const OceanView& view,
                               std::optional<double> ground, bool sea = true, bool tinted = false) {
    std::optional<Patch> patch;
    if (ground) {
        patch = Patch{.origin = WorldPosition{up * (radius + *ground)}, .vertices = flat_patch()};
    }
    return draw_patch(device, view, patch, sea, tinted);
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

TEST_CASE("ground at sea level lies under the sea, wherever the eye stands", "[render][gpu]") {
    const auto device = test_device();
    // Walking height, looking east and down at ground from 20 to 40 metres off, the sun behind.
    const auto view_from = [](double east_of_top) {
        const double turned = east_of_top / radius;
        const DVec3 here = (up * std::cos(turned)) + (east * std::sin(turned));
        const DVec3 eastward = (east * std::cos(turned)) - (up * std::sin(turned));
        OceanView view = seen_from(3.0, east, up, glm::normalize(up - east));
        const WorldPosition eye{here * (radius + 3.0)};
        const DVec3 forward = glm::normalize((eastward * 30.0) - (here * 3.0));
        view.camera = {.eye = eye,
                       .forward = forward,
                       .up = glm::normalize(here - (forward * glm::dot(here, forward))),
                       .vertical_fov = 0.2};
        view.sun = glm::normalize(here - eastward);
        view.bend = hog::terrain::bend_from(eye, radius, 0.0);
        return view;
    };

    // Ground a centimetre up stays dry, and what is level with the sea, or as near as drawing can
    // tell, lies under it.
    for (const double height : {1e-2, 1e-4, 0.0, -1e-3}) {
        const bool under = height < 1e-3;
        for (const double stand : {0.0, 0.13, 0.41, 0.77}) {
            const OceanView view = view_from(stand);
            const Patch ground = level_patch(stand + 30.0, 20.0, height);
            const auto dry = draw_patch(*device, view, ground, false, true);
            const auto wet = draw_patch(*device, view, ground, true, true);
            int seen = 0;
            int covered = 0;
            int unchanged = 0;
            for (std::uint32_t y = 0; y < extent.height; ++y) {
                for (std::uint32_t x = 0; x < extent.width; ++x) {
                    if (pixel_at(dry, x, y) == background) {
                        continue;
                    }
                    ++seen;
                    covered += pixel_at(wet, x, y)[2] > pixel_at(dry, x, y)[2] + 40 ? 1 : 0;
                    unchanged += pixel_at(wet, x, y) == pixel_at(dry, x, y) ? 1 : 0;
                }
            }
            INFO("ground " << height << " m, standing " << stand << " m east");
            CHECK(seen > 500);
            CHECK((under ? covered : unchanged) == seen);
        }
    }
    CHECK(device->validation_errors() == 0);
}
