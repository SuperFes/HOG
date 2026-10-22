#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/render/terrain.hpp>
#include <hog/shaders/shore_probe.hpp>
#include <hog/shaders/waves_probe.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/material.hpp>
#include <hog/terrain/shore.hpp>
#include <hog/terrain/stitching.hpp>
#include <hog/waves/foam.hpp>
#include <hog/waves/run_up.hpp>
#include <hog/waves/shoaling.hpp>
#include <hog/waves/tide.hpp>
#include <hog/waves/waves.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <optional>
#include <random>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
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

// The tide standing `tide` metres over the sea's mean, round the eye over depths of `depth`
// everywhere as the shore's grids keep them; and the waves running up the shore there, as
// DrawnWaves has them, their swash turned as far as `incident` and `surging` through the eye.
struct Tide {
    double tide = 0.0;
    float depth = 0.0F;
    glm::vec4 run_up{0.0F};
    float incident = 0.0F;
    float surging = 0.0F;
};

// Draws over a black background the ground, if any, tinted below sea level if asked, and then, if
// asked, the sea's surface, with the tide if any; reads it back.
std::vector<std::uint8_t> draw_patch(hog::render::Device& device, OceanView view,
                                     const std::optional<Patch>& ground, bool sea, bool tinted,
                                     std::optional<Tide> tide = std::nullopt) {
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
        if (tide) {
            view.waves.tide.w = static_cast<float>(tide->tide);
            view.waves.run_up = tide->run_up;
            view.waves.swash_incident.w = tide->incident;
            view.waves.swash_surging.w = tide->surging;
            const std::size_t texels = hog::terrain::shore_spacing.size() *
                                       hog::terrain::shore_size * hog::terrain::shore_size;
            REQUIRE(ocean->update_shore(
                commands, 0,
                {.frame = hog::terrain::shore_frame(view.bend.up, radius),
                 .depths = std::vector<float>(texels, tide->depth),
                 .still = std::vector<float>(texels, hog::terrain::shore_highest),
                 .fetches = std::vector<float>(texels * hog::terrain::fetch_headings, 0.0F)}));
        }
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, target->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        terrain->draw(
            commands, extent, *pool, draws,
            {.camera = view.camera, .sun = view.sun, .bend = view.bend, .sea = ocean->bindings()});
        if (sea) {
            CHECK(ocean->draw(commands, extent, view));
        }
        vkCmdEndRendering(commands);
    }));
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

// As draw_patch, with level ground, if any, `ground` metres above sea level under the eye.
std::vector<std::uint8_t> draw(hog::render::Device& device, const OceanView& view,
                               std::optional<double> ground, bool sea = true, bool tinted = false,
                               std::optional<Tide> tide = std::nullopt) {
    std::optional<Patch> patch;
    if (ground) {
        patch = Patch{.origin = WorldPosition{up * (radius + *ground)}, .vertices = flat_patch()};
    }
    return draw_patch(device, view, patch, sea, tinted, tide);
}

Pixel pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t first = 4 * ((std::size_t{y} * extent.width) + x);
    return {pixels.at(first), pixels.at(first + 1), pixels.at(first + 2), pixels.at(first + 3)};
}

constexpr double earth = 6'371'000.0;

// From walking height over open sea on an Earth-sized planet, looking level towards +X, the sky
// and the sea lit through the air.
std::vector<std::uint8_t> through_air(hog::render::Device& device, double sun_elevation) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(sky);
    auto ocean =
        hog::render::Ocean::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, true);
    REQUIRE(ocean);
    const WorldPosition eye{up * (earth + 2.0)};
    const double elevation = sun_elevation * std::numbers::pi / 180.0;
    const DVec3 sun = (east * std::cos(elevation)) + (up * std::sin(elevation));
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, earth, 0.0);
    const hog::render::Camera camera{.eye = eye, .forward = east, .up = up};
    const hog::render::SkyView seen{.camera = camera, .sun = sun, .bend = bend, .exposure = 10.0};
    const OceanView view{
        .camera = camera, .sun = sun, .bend = bend, .air = sky->bindings(0, *atmosphere)};

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
        sky->draw(commands, extent, 0, *atmosphere, seen);
        CHECK(ocean->draw(commands, extent, view));
        vkCmdEndRendering(commands);
    }));
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

// What lies beneath the sea's surface.
enum class Beneath : std::uint8_t { Seabed, Deep, Nothing };

// From `height` metres up, looking east and steeply down through the air at the sea over ground
// four kilometres beneath it, too deep to show, drawn either as the ground or as the deep water; or
// over nothing.
std::vector<std::uint8_t> over_deep_sea(hog::render::Device& device, Beneath beneath,
                                        double height = 20'000.0) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(sky);
    auto ocean =
        hog::render::Ocean::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, true);
    REQUIRE(ocean);
    const std::vector<hog::terrain::MaterialImage> materials(
        hog::planet::biome_count,
        hog::terrain::material_image({.colour = DVec3{0.0, 0.75, 0.25}}, 64));
    auto terrain =
        hog::render::Terrain::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                                     {.materials = materials, .sea = true, .air = true});
    REQUIRE(terrain);
    auto pool = hog::render::TerrainPool::create(device, {});
    REQUIRE(pool);
    const Patch seabed = level_patch(40'000.0, 160'000.0, -4'000.0);
    const std::uint32_t slot = pool->acquire().value_or(1);
    REQUIRE(pool->stage(0, slot, seabed.vertices));
    const std::vector<hog::render::TerrainDraw> draws{{.slot = slot, .origin = seabed.origin}};

    const WorldPosition eye{up * (radius + height)};
    // From under the sea, steeply up at its surface.
    const double steep = height > 0.0 ? 3.0 : -3.0;
    const DVec3 sun = glm::normalize(up + north);
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, 0.0);
    const hog::render::Camera camera{.eye = eye,
                                     .forward = glm::normalize(east - (up * steep)),
                                     .up = glm::normalize((east * steep) + up)};
    const hog::render::SkyView seen{.camera = camera, .sun = sun, .bend = bend, .exposure = 10.0};
    const hog::render::AirBindings air = sky->bindings(0, *atmosphere);
    const OceanView view{.camera = camera, .sun = sun, .bend = bend, .air = air};

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
        CHECK(sky->record_frame(commands, extent, 0, *atmosphere, seen));
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, target->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        sky->draw(commands, extent, 0, *atmosphere, seen);
        if (beneath == Beneath::Seabed) {
            terrain->draw(commands, extent, *pool, draws,
                          {.camera = camera, .sun = sun, .bend = bend, .air = air});
        } else if (beneath == Beneath::Deep) {
            CHECK(ocean->draw_deep(commands, extent, view));
        }
        CHECK(ocean->draw(commands, extent, view));
        vkCmdEndRendering(commands);
    }));
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

int brightness(Pixel pixel) {
    return pixel[0] + pixel[1] + pixel[2];
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

TEST_CASE("the tide floods low ground the sea reaches, and bares ground under its mean",
          "[render][gpu]") {
    const auto device = test_device();
    const OceanView view = seen_from(4.0, -up, north, glm::normalize(east + up));
    const std::uint32_t middle = extent.width / 2;
    const auto seen = [&](double ground, bool sea, std::optional<Tide> tide) {
        return pixel_at(draw(*device, view, ground, sea, true, tide), middle, middle);
    };
    // Half a metre above the sea's mean: dry with the tide out, and with it a metre in as deep
    // under it as ground half a metre below the mean is at the mean, unless the sea can't reach it.
    const Pixel dry = seen(0.5, false, std::nullopt);
    CHECK(seen(0.5, true, Tide{.tide = 0.0, .depth = -0.5F}) == dry);
    const Pixel flooded = seen(0.5, true, Tide{.tide = 1.0, .depth = -0.5F});
    CHECK(flooded != dry);
    CHECK(flooded == seen(-0.5, true, Tide{.tide = 0.0, .depth = 0.5F}));
    CHECK(seen(0.5, true, Tide{.tide = 1.0, .depth = hog::terrain::shore_highest}) == dry);
    // Half a metre below it, bared with the tide well out.
    CHECK(seen(-0.5, true, Tide{.tide = -1.0, .depth = 0.5F}) ==
          pixel_at(draw(*device, view, -0.5, false, false), middle, middle));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the swash runs up the shore and draws back, leaving the sand wet", "[render][gpu]") {
    const auto device = test_device();
    const OceanView view = seen_from(4.0, -up, north, glm::normalize(east + up));
    const std::uint32_t middle = extent.width / 2;
    // Two-metre waves of ten seconds, as drawn_waves_at would have them, on the gentlest beach:
    // they set the sea up a few centimetres and its swash runs half a metre above and below that.
    const double scale = std::sqrt(2.0 * hog::waves::gravity * 100.0 / (2.0 * std::numbers::pi));
    const glm::vec4 waves{static_cast<float>(scale), 3.4F, 0.63F,
                          static_cast<float>(0.63 / hog::waves::surging)};
    const auto seen = [&](double ground, float surging) {
        return pixel_at(draw(*device, view, ground, true, true,
                             Tide{.depth = static_cast<float>(-ground),
                                  .run_up = waves,
                                  .incident = 2.0F,
                                  .surging = surging}),
                        middle, middle);
    };
    const auto dry = [&](double ground) {
        return pixel_at(draw(*device, view, ground, false, true), middle, middle);
    };
    // Four tenths of a metre up the beach: under the swash as it surges up, bare as it draws back
    // but darker for its having just been under it.
    const Pixel up_beach = dry(0.4);
    const Pixel surged = seen(0.4, 0.0F);
    CHECK(surged != up_beach);
    const Pixel drawn_back = seen(0.4, static_cast<float>(std::numbers::pi));
    CHECK(drawn_back[1] + 20 < up_beach[1]);
    CHECK(drawn_back[0] <= up_beach[0]);
    // Above where it ever runs, the sand stays dry.
    CHECK(seen(2.0, 0.0F) == dry(2.0));
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

TEST_CASE("through the air, the sea meets the sky at the horizon without a seam",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    // The sun behind, so neither its disc nor its glint is in view.
    const auto pixels = through_air(*device, -150.0);
    const auto daytime = through_air(*device, 160.0);
    const std::uint32_t middle = extent.width / 2;
    const std::uint32_t horizon = extent.height / 2;
    const Pixel above = pixel_at(daytime, middle, horizon - 1);
    const Pixel below = pixel_at(daytime, middle, horizon + 1);
    INFO("above " << int{above[0]} << ", " << int{above[1]} << ", " << int{above[2]} << "; below "
                  << int{below[0]} << ", " << int{below[1]} << ", " << int{below[2]});
    // Just below, it reflects most of the sky just above.
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CHECK(below.at(channel) <= above.at(channel));
        CHECK(below.at(channel) >= 0.8 * above.at(channel));
    }
    // The steeper the view down, the less the sea reflects.
    CHECK(brightness(pixel_at(daytime, middle, extent.height - 1)) < brightness(below));
    CHECK(below[2] > below[0]);
    // At night, it's dark.
    CHECK(brightness(pixel_at(pixels, middle, horizon + 1)) < 15);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("through the air, the sun's glint on the sea reddens as it sets",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const auto setting = through_air(*device, 2.0);
    const std::uint32_t middle = extent.width / 2;
    Pixel brightest{};
    for (std::uint32_t y = (extent.height / 2) + 1; y < extent.height; ++y) {
        if (brightness(pixel_at(setting, middle, y)) > brightness(brightest)) {
            brightest = pixel_at(setting, middle, y);
        }
    }
    const Pixel aside = pixel_at(setting, 2, (extent.height / 2) + 4);
    INFO("glint " << int{brightest[0]} << ", " << int{brightest[1]} << ", " << int{brightest[2]}
                  << "; aside " << int{aside[0]} << ", " << int{aside[1]} << ", " << int{aside[2]});
    CHECK(brightness(brightest) > 2 * brightness(aside));
    CHECK(brightest[0] > 2 * brightest[2]);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("through the air, the deep water drawn beneath the sea looks as ground too deep to show",
          "[render][gpu]") {
    const auto device = test_device();
    const auto seabed = over_deep_sea(*device, Beneath::Seabed);
    const auto deep = over_deep_sea(*device, Beneath::Deep);
    const auto nothing = over_deep_sea(*device, Beneath::Nothing);
    int furthest = 0;
    int without = 0;
    for (std::uint32_t x = 0; x < extent.width; ++x) {
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            const Pixel drawn = pixel_at(seabed, x, y);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                furthest = std::max(furthest,
                                    std::abs(drawn.at(channel) - pixel_at(deep, x, y).at(channel)));
                without = std::max(
                    without, std::abs(drawn.at(channel) - pixel_at(nothing, x, y).at(channel)));
            }
        }
    }
    CAPTURE(furthest);
    CHECK(furthest <= 2);
    // Without it, the sky would show through.
    CHECK(without > 20);

    // From under the sea, the sky shows through its surface overhead as before.
    CHECK(over_deep_sea(*device, Beneath::Deep, -20.0) ==
          over_deep_sea(*device, Beneath::Nothing, -20.0));
    CHECK(device->validation_errors() == 0);
}

namespace {

// Late in 2026, so phases are taken far from the epoch, as they will be.
constexpr hog::GameTime noon{std::chrono::seconds{1'792'000'000}};

// Where the drawn waves move points of sea level `rest` from the eye, seen over `footprint`
// metres each, and how the surface faces there; how rough what's left is; how much of it is white
// breaking and with foam left; and the foam's coarse and fine pattern there, and how much of the
// fine is seen, which is none where it couldn't make any of it white.
struct Probed {
    DVec3 moved;
    DVec3 normal;
    double roughness;
    double breaking;
    double left;
    double coarse;
    double fine;
    double seen;
    // The root mean square of how high the drawn waves rise, and the share white in the surf.
    double heave;
    double surf;
};

// Over floors as wave_surface takes them, one for each point, or deep water; and over still water
// with as much fetch, one for each point, or the sea.
std::vector<Probed> probe_waves(hog::render::Device& device, const hog::render::DrawnWaves& drawn,
                                std::span<const DVec3> rest, double footprint,
                                std::span<const hog::Vec3> floors = {},
                                std::span<const float> fetches = {}) {
    std::vector<std::array<float, 4>> asked;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const DVec3& point = rest[i];
        asked.push_back({static_cast<float>(point.x), static_cast<float>(point.y),
                         static_cast<float>(point.z), static_cast<float>(footprint)});
        const hog::Vec3 floor = floors.empty() ? hog::Vec3{1.0e6F, 0.0F, 0.0F} : floors[i];
        asked.push_back({floor.x, floor.y, floor.z, fetches.empty() ? 0.0F : fetches[i]});
    }
    constexpr auto binding = [](std::uint32_t at, VkDescriptorType type) {
        return VkDescriptorSetLayoutBinding{.binding = at,
                                            .descriptorType = type,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = nullptr};
    };
    const std::array bindings{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                              binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                              binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
    auto probe = hog::render::make_compute_pipeline(
        device, {.spirv = hog::shaders::waves_probe, .descriptors = bindings});
    REQUIRE(probe);
    auto waves = hog::render::make_filled_buffer(device, std::as_bytes(std::span{&drawn, 1}),
                                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    REQUIRE(waves);
    auto questions = hog::render::make_filled_buffer(device, std::as_bytes(std::span{asked}),
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(questions);
    std::vector<std::array<float, 4>> found(4 * rest.size());
    auto answers =
        hog::render::make_buffer(device, std::span{found}.size_bytes(),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(answers);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe->handle());
        hog::render::push_uniform_buffer(commands, *probe, 0, waves->handle());
        hog::render::push_storage_buffer(commands, *probe, 1, questions->handle());
        hog::render::push_storage_buffer(commands, *probe, 2, answers->handle());
        vkCmdDispatch(commands, static_cast<std::uint32_t>((rest.size() + 63) / 64), 1, 1);
        const VkMemoryBarrier2 to_host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .pNext = nullptr,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                       .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .pNext = nullptr,
                                          .dependencyFlags = 0,
                                          .memoryBarrierCount = 1,
                                          .pMemoryBarriers = &to_host,
                                          .bufferMemoryBarrierCount = 0,
                                          .pBufferMemoryBarriers = nullptr,
                                          .imageMemoryBarrierCount = 0,
                                          .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(commands, &dependency);
    }));
    REQUIRE(answers->invalidate());
    std::memcpy(found.data(), answers->mapped().data(), std::span{found}.size_bytes());
    std::vector<Probed> probed;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const auto& moved = found.at(4 * i);
        const auto& normal = found.at((4 * i) + 1);
        const auto& foam = found.at((4 * i) + 2);
        const auto& surf = found.at((4 * i) + 3);
        probed.push_back({.moved = DVec3{hog::Vec3{moved[0], moved[1], moved[2]}},
                          .normal = DVec3{hog::Vec3{normal[0], normal[1], normal[2]}},
                          .roughness = static_cast<double>(moved[3]),
                          .breaking = static_cast<double>(normal[3]),
                          .left = static_cast<double>(foam[0]),
                          .coarse = static_cast<double>(foam[1]),
                          .fine = static_cast<double>(foam[2]),
                          .seen = static_cast<double>(foam[3]),
                          .heave = static_cast<double>(surf[0]),
                          .surf = static_cast<double>(surf[1])});
    }
    return probed;
}

// Eight metres over a sea raised by a wind of `wind` m/s, somewhere off the top of the planet.
struct WavySea {
    std::vector<hog::waves::Train> trains = hog::waves::planet_trains(5);
    WorldPosition eye{glm::normalize(DVec3{0.03, -0.02, 1.0}) * (radius + 8.0)};
    hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, 0.0);
    hog::waves::SeaState sea;
    std::vector<hog::waves::Wave> waves;
    hog::render::DrawnWaves drawn;

    explicit WavySea(double wind) {
        const DVec3 along = glm::normalize(glm::cross(bend.up, north));
        sea = {
            .systems = {hog::waves::grown_sea(along * wind)}, .current = {}, .wind = along * wind};
        waves = hog::waves::waves_at(trains, bend.up, sea);
        drawn = hog::render::drawn_waves_at(trains, waves, sea, eye, bend, noon, DVec3{0.0});
    }

    // Sea level `out` metres from under the eye along the ground, from the eye.
    [[nodiscard]] DVec3 rest(DVec3 out) const {
        const DVec3 flat = out - (bend.up * glm::dot(out, bend.up));
        const DVec3 under = bend.up * radius;
        return (glm::normalize(under + flat) * radius) - eye.metres();
    }
};

} // namespace

TEST_CASE("the sea's drawn waves move and face it as hog::waves has them", "[render][gpu]") {
    const auto device = test_device();
    const WavySea sea{9.0};
    REQUIRE(sea.drawn.count_left_radius.x == static_cast<float>(hog::render::drawn_waves));
    // The strongest as they show under the eye, and those as hog::waves takes them.
    std::vector<hog::waves::Wave> shown;
    for (const hog::waves::Wave& wave : sea.waves) {
        const double showing =
            std::sqrt(hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
        if (showing > 0.0) {
            shown.push_back({.train = wave.train, .amplitude = wave.amplitude * showing});
        }
    }
    std::vector<hog::waves::Wave> kept =
        hog::waves::strongest(sea.trains, shown, hog::render::drawn_waves).waves;
    for (hog::waves::Wave& wave : kept) {
        wave.amplitude /=
            std::sqrt(hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
    }
    std::mt19937_64 random{2};
    std::uniform_real_distribution<double> spot{-150.0, 150.0};
    std::vector<DVec3> rest;
    rest.reserve(100);
    for (int i = 0; i < 100; ++i) {
        rest.push_back(sea.rest(DVec3{spot(random), spot(random), 0.0}));
    }
    const std::vector<Probed> probed = probe_waves(*device, sea.drawn, rest, 1e-3);
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const hog::waves::Surface surface =
            hog::waves::surface_at(sea.trains, kept, sea.eye.metres() + rest[i], noon);
        CAPTURE(i);
        CHECK(glm::length(probed[i].moved - surface.moved) < 0.01);
        CHECK(glm::dot(probed[i].normal, surface.normal) > 0.9999);
        CHECK(probed[i].roughness == static_cast<double>(sea.drawn.count_left_radius.y));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("still water rises only in the waves the wind has grown over it", "[render][gpu]") {
    const auto device = test_device();
    // Inland, with no sea, under a wind of 9 m/s.
    WavySea sea{9.0};
    const double wind = glm::length(sea.sea.wind);
    sea.sea.systems.clear();
    sea.waves.clear();
    sea.drawn = hog::render::drawn_waves_at(sea.trains, sea.waves, sea.sea, sea.eye, sea.bend, noon,
                                            DVec3{0.0});
    // The strongest as they'd run grown full under the wind, as hog::waves takes them.
    const hog::waves::SeaState grown{
        .systems = {hog::waves::grown_sea(sea.sea.wind)}, .current = {}, .wind = sea.sea.wind};
    const auto showing = [&](const hog::waves::Wave& wave) {
        return std::sqrt(
            hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
    };
    std::vector<hog::waves::Wave> shown;
    for (const hog::waves::Wave& wave : hog::waves::waves_at(sea.trains, sea.bend.up, grown)) {
        if (showing(wave) > 0.0) {
            shown.push_back({.train = wave.train, .amplitude = wave.amplitude * showing(wave)});
        }
    }
    const std::vector<hog::waves::Wave> kept =
        hog::waves::strongest(sea.trains, shown, hog::render::drawn_waves).waves;
    REQUIRE(sea.drawn.count_left_radius.x == static_cast<float>(kept.size()));
    std::mt19937_64 random{3};
    std::uniform_real_distribution<double> spot{-100.0, 100.0};
    std::vector<DVec3> rest;
    rest.reserve(40);
    for (int i = 0; i < 40; ++i) {
        rest.push_back(sea.rest(DVec3{spot(random), spot(random), 0.0}));
    }
    const std::vector<hog::Vec3> deep(rest.size(), hog::Vec3{1.0e6F, 0.0F, 0.0F});
    double lower = 0.0;
    for (const float fetch : {40.0F, 400.0F, 4'000.0F}) {
        CAPTURE(fetch);
        std::vector<hog::waves::Wave> fetched;
        for (const hog::waves::Wave& wave : kept) {
            const hog::waves::Train& train = sea.trains[wave.train];
            fetched.push_back({.train = wave.train,
                               .amplitude = wave.amplitude / showing(wave) *
                                            hog::waves::fetched(train.frequency,
                                                                static_cast<double>(fetch), wind)});
        }
        // What's left out of each band, as grown at its middle.
        double left = 0.0;
        for (int band = 0; band < hog::waves::bands; ++band) {
            const double growth = hog::waves::fetched(
                std::sqrt(hog::waves::band_lowest(band) * hog::waves::band_lowest(band + 1)),
                static_cast<double>(fetch), wind);
            left += static_cast<double>(sea.drawn.still_left.at(
                        static_cast<std::size_t>(band / 4))[static_cast<glm::length_t>(band % 4)]) *
                    growth * growth;
        }
        const std::vector<float> fetches(rest.size(), fetch);
        const std::vector<Probed> probed =
            probe_waves(*device, sea.drawn, rest, 1e-3, deep, fetches);
        double heave = 0.0;
        for (std::size_t i = 0; i < rest.size(); ++i) {
            const hog::waves::Surface surface =
                hog::waves::surface_at(sea.trains, fetched, sea.eye.metres() + rest[i], noon);
            CAPTURE(i);
            CHECK(glm::length(probed[i].moved - surface.moved) < 0.005);
            CHECK(glm::dot(probed[i].normal, surface.normal) > 0.9999);
            CHECK_THAT(probed[i].roughness, WithinRel(left, 1e-3));
            heave += probed[i].heave;
        }
        // The further the wind has blown over it, the higher it rises.
        CHECK(heave > lower * 1.5);
        lower = heave;
        // Seen from afar, all of it is roughness, each wave grown as far as its own fetch lets it.
        double all = left;
        for (const hog::waves::Wave& wave : kept) {
            const hog::waves::Train& train = sea.trains[wave.train];
            const double rising = glm::dot(sea.bend.up, train.heading);
            const double slope =
                train.wavenumber * std::sqrt(1.0 - (rising * rising)) * wave.amplitude *
                hog::waves::fetched(train.frequency, static_cast<double>(fetch), wind);
            all += slope * slope / 2.0;
        }
        const Probed far = probe_waves(*device, sea.drawn, std::span{rest}.first(1), 1e4,
                                       std::span{deep}.first(1), std::span{fetches}.first(1))
                               .front();
        CHECK_THAT(far.roughness, WithinRel(all, 1e-3));
        CHECK(glm::length(far.moved) == 0.0);
    }
    // Over the sea, where there's none, it lies flat.
    for (const Probed& probed : probe_waves(*device, sea.drawn, rest, 1e-3, deep)) {
        CHECK(glm::length(probed.moved) < 1e-6);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("over a shore the sea's drawn waves shoal as hog::waves has them", "[render][gpu]") {
    const auto device = test_device();
    // A swell running in at a shore lying east of the eye, and the wind sea.
    WavySea sea{9.0};
    const DVec3 first{hog::Vec3{sea.drawn.first}};
    sea.sea.systems.push_back({.heading = glm::normalize(-first + (north * 0.4)),
                               .height = 1.5,
                               .peak = 0.55,
                               .peakedness = hog::waves::swell_peakedness,
                               .spread = hog::waves::swell_spread});
    sea.waves = hog::waves::waves_at(sea.trains, sea.bend.up, sea.sea);
    sea.drawn = hog::render::drawn_waves_at(sea.trains, sea.waves, sea.sea, sea.eye, sea.bend, noon,
                                            DVec3{0.0});
    std::vector<hog::waves::Wave> shown;
    for (const hog::waves::Wave& wave : sea.waves) {
        const double showing =
            std::sqrt(hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
        if (showing > 0.0) {
            shown.push_back({.train = wave.train, .amplitude = wave.amplitude * showing});
        }
    }
    std::vector<hog::waves::Wave> kept =
        hog::waves::strongest(sea.trains, shown, hog::render::drawn_waves).waves;
    for (hog::waves::Wave& wave : kept) {
        wave.amplitude /=
            std::sqrt(hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
    }
    // From the waterline 300 m east out past the shelf's turn; not so far that the precision
    // floats keep there is what's tested.
    std::vector<DVec3> rest;
    std::vector<hog::Vec3> floors;
    std::vector<hog::waves::Floor> cpu_floors;
    for (const double west : {295.0, 280.0, 240.0, 150.0, 0.0, -1'500.0}) {
        for (const double across : {-20.0, 35.0}) {
            const double out = 300.0 - west;
            const double depth = std::max(hog::waves::dean_scale * std::cbrt(out * out),
                                          out / hog::waves::shelf_run);
            const double deepening = depth < hog::waves::shelf_depth
                                         ? 2.0 / 3.0 * hog::waves::dean_scale / std::cbrt(out)
                                         : 1.0 / hog::waves::shelf_run;
            rest.push_back(sea.rest((first * west) + (north * across)));
            floors.emplace_back(static_cast<float>(depth), static_cast<float>(-deepening), 0.0F);
            cpu_floors.push_back({.depth = depth, .deepening = -first * deepening});
        }
    }
    const std::vector<Probed> probed = probe_waves(*device, sea.drawn, rest, 1e-3, floors);
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const hog::waves::Surface surface = hog::waves::surface_at(
            sea.trains, kept, sea.eye.metres() + rest[i], noon, cpu_floors[i]);
        CAPTURE(i, cpu_floors[i].depth);
        CHECK(glm::length(probed[i].moved - surface.moved) <
              0.01 + (0.01 * glm::length(surface.moved)));
        CHECK(glm::dot(probed[i].normal, surface.normal) > 0.999);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the sea's drawn waves carry the tide round the eye", "[render]") {
    // Eight metres over a calm sea at middling latitudes, where the tide runs high.
    const std::vector<hog::waves::Train> trains = hog::waves::planet_trains(5);
    const WorldPosition eye{glm::normalize(DVec3{0.6, 0.5, 0.4}) * (radius + 8.0)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, 0.0);
    const hog::waves::SeaState calm{};
    constexpr hog::GameDuration day = std::chrono::hours{4};
    for (int hour = 0; hour < 8; ++hour) {
        const hog::GameTime time = noon + std::chrono::minutes{17 * hour};
        const hog::render::DrawnWaves drawn =
            hog::render::drawn_waves_at(trains, {}, calm, eye, bend, time, DVec3{0.0}, day);
        CHECK_THAT(static_cast<double>(drawn.tide.w),
                   WithinAbs(hog::waves::tide_at(bend.up, time, day), 1e-6));
        // As it rises a kilometre off along the ground.
        const DVec3 off = glm::normalize(north - (bend.up * glm::dot(north, bend.up)));
        const double there =
            hog::waves::tide_at(glm::normalize(bend.up + (off * (1'000.0 / radius))), time, day);
        CHECK_THAT(static_cast<double>(drawn.tide.w) +
                       (1'000.0 * glm::dot(DVec3{hog::Vec3{drawn.tide}}, off)),
                   WithinAbs(there, 1e-3));
    }
    CHECK(hog::render::drawn_waves_at(trains, {}, calm, eye, bend, noon, DVec3{0.0}).tide ==
          glm::vec4{0.0F});
}

TEST_CASE("the sea's drawn waves carry their run-up, its swash fixed to the world", "[render]") {
    const WavySea sea{9.0};
    const hog::render::DrawnWaves drawn = sea.drawn;
    // As waves::run_up scales it, from the drawn waves' significant height and peak period.
    CHECK(drawn.run_up.x > 0.0F);
    CHECK(drawn.run_up.y > 0.0F);
    CHECK_THAT(static_cast<double>(drawn.run_up.w),
               WithinRel(static_cast<double>(drawn.run_up.z) / hog::waves::surging, 1e-6));
    // From an eye a few metres on, the swash lies where it did.
    const DVec3 step = glm::normalize(east - (sea.bend.up * glm::dot(east, sea.bend.up))) * 7.0;
    const WorldPosition moved{sea.eye.metres() + step};
    const hog::render::DrawnWaves there =
        hog::render::drawn_waves_at(sea.trains, sea.waves, sea.sea, moved,
                                    hog::terrain::bend_from(moved, radius, 0.0), noon, DVec3{0.0});
    for (const auto& [here, after] : {std::pair{drawn.swash_incident, there.swash_incident},
                                      std::pair{drawn.swash_surging, there.swash_surging}}) {
        const double expected =
            static_cast<double>(here.w) + glm::dot(DVec3{hog::Vec3{here}}, step);
        CHECK_THAT(std::remainder(static_cast<double>(after.w) - expected, 2.0 * std::numbers::pi),
                   WithinAbs(0.0, 1e-3));
    }
}

TEST_CASE("waves too short to see from afar fade into the sea's roughness", "[render][gpu]") {
    const auto device = test_device();
    const WavySea sea{9.0};
    const std::vector<DVec3> rest{sea.rest(DVec3{40.0, 0.0, 0.0})};
    const Probed near = probe_waves(*device, sea.drawn, rest, 1e-3).front();
    const Probed far = probe_waves(*device, sea.drawn, rest, 5.0).front();
    const Probed farther = probe_waves(*device, sea.drawn, rest, 1e4).front();
    const auto all = static_cast<double>(sea.drawn.count_left_radius.y +
                                         sea.drawn.waves.front().amplitude_phase_slope_heave.z);
    CHECK(near.roughness < far.roughness);
    CHECK(far.roughness < farther.roughness);
    // Seen from afar all of it is roughness, and what showed of the shape is gone.
    CHECK(std::abs(farther.roughness - all) < all * 1e-3);
    CHECK(glm::length(farther.moved) == 0.0);
    CHECK(glm::length(far.moved) > 0.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("crests break white over as much of the sea as the wind breaks", "[render][gpu]") {
    const auto device = test_device();
    const WavySea sea{20.0};
    const auto breaking = static_cast<double>(sea.drawn.foam.z);
    const auto left = static_cast<double>(sea.drawn.foam.w);
    REQUIRE(breaking > 0.01);
    REQUIRE(left > breaking);
    // Close by, each place is white or not; over kilometres, as much as the wind makes white.
    std::mt19937_64 random{3};
    std::uniform_real_distribution<double> spot{-3'000.0, 3'000.0};
    std::vector<DVec3> rest;
    rest.reserve(4'000);
    for (int i = 0; i < 4'000; ++i) {
        rest.push_back(sea.rest(DVec3{spot(random), spot(random), 0.0}));
    }
    double mean_breaking = 0.0;
    double mean_left = 0.0;
    double coarse = 0.0;
    double coarse_squared = 0.0;
    int unsure = 0;
    for (const Probed& probed : probe_waves(*device, sea.drawn, rest, 1e-3)) {
        mean_breaking += probed.breaking;
        mean_left += probed.left;
        coarse += probed.coarse;
        coarse_squared += probed.coarse * probed.coarse;
        unsure += probed.breaking > 0.05 && probed.breaking < 0.95 ? 1 : 0;
        if (probed.breaking + probed.left > 0.01) {
            CHECK_THAT(probed.seen, WithinRel(1.0, 1e-3));
        }
    }
    const auto count = static_cast<double>(rest.size());
    CHECK_THAT(mean_breaking / count, WithinRel(breaking, 0.1));
    CHECK_THAT(mean_left / count, WithinRel(left, 0.15));
    CHECK(std::abs(coarse / count) < 0.1);
    CHECK_THAT(coarse_squared / count, WithinRel(1.0, 0.1));
    // Whitecaps fade out over their edges, which are less of them than what's white through.
    CHECK(static_cast<double>(unsure) < count * breaking);
    // From afar, each footprint is that much white.
    const std::vector<DVec3> far{sea.rest(DVec3{0.0, 0.0, 0.0}), sea.rest(DVec3{900.0, 0.0, 0.0})};
    for (const Probed& probed : probe_waves(*device, sea.drawn, far, 1e5)) {
        CHECK(probed.seen == 0.0);
        CHECK_THAT(probed.breaking, WithinRel(breaking, 0.02));
        CHECK_THAT(probed.left, WithinRel(left, 0.02));
    }
    // A breeze whitens next to nothing.
    const WavySea breeze{2.0};
    CHECK(breeze.drawn.foam.z < 1e-4F);
    int white = 0;
    int fine = 0;
    for (const Probed& probed : probe_waves(*device, breeze.drawn, rest, 1e-3)) {
        white += probed.breaking + probed.left > 0.5 ? 1 : 0;
        fine += probed.seen > 0.0 ? 1 : 0;
    }
    CHECK(white < 4);
    // The fine pattern is only taken where there might be foam.
    CHECK(static_cast<double>(fine) < count / 10.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("in the surf, as much of the sea breaks white as its waves break, wind or none",
          "[render][gpu]") {
    const auto device = test_device();
    // A swell running in over a shore under a breeze too light to whiten the sea.
    WavySea sea{3.0};
    const DVec3 first{hog::Vec3{sea.drawn.first}};
    sea.sea.systems.push_back({.heading = glm::normalize(first + (north * 0.4)),
                               .height = 1.5,
                               .peak = 0.55,
                               .peakedness = hog::waves::swell_peakedness,
                               .spread = hog::waves::swell_spread});
    sea.waves = hog::waves::waves_at(sea.trains, sea.bend.up, sea.sea);
    sea.drawn = hog::render::drawn_waves_at(sea.trains, sea.waves, sea.sea, sea.eye, sea.bend, noon,
                                            DVec3{0.0});
    REQUIRE(sea.drawn.foam.z < 1e-4F);
    std::mt19937_64 random{4};
    std::uniform_real_distribution<double> spot{-1'500.0, 1'500.0};
    std::vector<DVec3> rest;
    rest.reserve(4'000);
    for (int i = 0; i < 4'000; ++i) {
        rest.push_back(sea.rest(DVec3{spot(random), spot(random), 0.0}));
    }
    for (const double depth : {0.8, 1.5, 3.0, 6.0}) {
        CAPTURE(depth);
        const double deepening =
            2.0 / 3.0 * hog::waves::dean_scale / std::sqrt(depth / hog::waves::dean_scale);
        const std::vector<hog::Vec3> floors(
            rest.size(),
            hog::Vec3{static_cast<float>(depth), static_cast<float>(-deepening), 0.0F});
        double breaking = 0.0;
        double left = 0.0;
        double expected_breaking = 0.0;
        double expected_left = 0.0;
        for (const Probed& probed : probe_waves(*device, sea.drawn, rest, 1e-3, floors)) {
            const double height = std::sqrt(8.0) * probed.heave;
            CHECK_THAT(probed.surf, WithinRel(hog::waves::breaking_waves(height, depth), 1e-3) ||
                                        WithinAbs(0.0, 1e-9));
            const hog::waves::Whitecaps surf = hog::waves::surf(height, depth);
            breaking += probed.breaking;
            left += probed.left;
            expected_breaking += surf.breaking;
            expected_left += surf.left;
        }
        // Close by, each place is white or not; over the shore, as much as breaks. Where little
        // does, only near it: a swell's few trains rise less often far over their mean than a
        // normal sea.
        const auto count = static_cast<double>(rest.size());
        CHECK(expected_breaking > 0.0);
        CHECK_THAT(breaking / count, WithinRel(expected_breaking / count, 0.1) ||
                                         WithinAbs(expected_breaking / count, 0.003));
        CHECK_THAT(left / count, WithinRel(expected_left / count, 0.1) ||
                                     WithinAbs(expected_left / count, 0.003));
        // From afar, each footprint is that much white.
        const std::vector<DVec3> far{rest.front(), rest.back()};
        const std::vector<hog::Vec3> far_floors(far.size(), floors.front());
        for (const Probed& probed : probe_waves(*device, sea.drawn, far, 1e5, far_floors)) {
            // Or as much as the breeze whitens, if more.
            const hog::waves::Whitecaps surf =
                hog::waves::surf(std::sqrt(8.0) * probed.heave, depth);
            CHECK(probed.seen == 0.0);
            CHECK_THAT(
                probed.breaking,
                WithinRel(std::max(surf.breaking, static_cast<double>(sea.drawn.foam.z)), 0.02));
            CHECK_THAT(probed.left,
                       WithinRel(std::max(surf.left, static_cast<double>(sea.drawn.foam.w)), 0.02));
        }
    }
    // Out where it's deep, the breeze whitens next to nothing.
    int white = 0;
    for (const Probed& probed : probe_waves(*device, sea.drawn, rest, 1e-3)) {
        white += probed.breaking + probed.left > 0.5 ? 1 : 0;
        CHECK(probed.surf == 0.0);
    }
    CHECK(white < 4);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the sea's foam drifts with the water at its surface", "[render][gpu]") {
    const auto device = test_device();
    const WavySea sea{20.0};
    const DVec3 drifted = glm::normalize(glm::cross(sea.bend.up, east)) * 37.0;
    const hog::render::DrawnWaves moved = hog::render::drawn_waves_at(
        sea.trains, sea.waves, sea.sea, sea.eye, sea.bend, noon, drifted);
    std::vector<DVec3> there;
    std::vector<DVec3> before;
    for (int i = 0; i < 20; ++i) {
        const DVec3 out{(i * 7.0) - 60.0, (i % 5) * 11.0, 0.0};
        there.push_back(sea.rest(out));
        before.push_back(there.back() - drifted);
    }
    const std::vector<Probed> now = probe_waves(*device, moved, there, 1e-3);
    const std::vector<Probed> then = probe_waves(*device, sea.drawn, before, 1e-3);
    for (std::size_t i = 0; i < now.size(); ++i) {
        CAPTURE(i);
        CHECK_THAT(now[i].coarse, WithinAbs(then[i].coarse, 1e-3));
        if (now[i].seen > 0.0 && then[i].seen > 0.0) {
            CHECK_THAT(now[i].fine, WithinAbs(then[i].fine, 1e-3));
        }
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("waves scatter the sun's glint over the sea", "[render][gpu]") {
    const auto device = test_device();
    // Looking down with the sun overhead.
    OceanView view = seen_from(4.0, -up, north, up);
    const auto calm = draw(*device, view, std::nullopt);
    const std::vector<hog::waves::Train> trains = hog::waves::planet_trains(5);
    const hog::waves::SeaState windy{
        .systems = {hog::waves::grown_sea(east * 9.0)}, .current = {}, .wind = {}};
    view.waves = hog::render::drawn_waves_at(trains, hog::waves::waves_at(trains, up, windy), windy,
                                             view.camera.eye, view.bend, noon, DVec3{0.0});
    REQUIRE(view.waves.count_left_radius.x > 0.0F);
    const auto wavy = draw(*device, view, std::nullopt);
    const auto glinting = [](const std::vector<std::uint8_t>& pixels) {
        int lit = 0;
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            for (std::uint32_t x = 0; x < extent.width; ++x) {
                lit += pixel_at(pixels, x, y)[0] > 10 ? 1 : 0;
            }
        }
        return lit;
    };
    // Glittering over more of it, each glint dimmer.
    CAPTURE(glinting(calm), glinting(wavy));
    CHECK(glinting(wavy) > 4 * glinting(calm));
    const std::uint32_t middle = extent.width / 2;
    CHECK(pixel_at(wavy, middle, middle)[0] < pixel_at(calm, middle, middle)[0]);
    CHECK(device->validation_errors() == 0);
}

namespace {

// What the ocean's shaders find over the plane of `frame` round an eye 5 m up at its anchor.
struct ShoreFound {
    double depth;
    double still;
    double fetch;
};

// Of fields over the plane of `frame`, each by metres east and north on it: of the sea's depths,
// still water's and the fetch towards each heading. What the ocean's shaders find under each of
// `points`, the fetch for a wind blowing towards `wind`.
std::vector<ShoreFound> probe_shore(hog::render::Device& device,
                                    const hog::terrain::ShoreFrame& frame, const auto& depth_at,
                                    const auto& still_at, const auto& fetch_at, DVec3 wind,
                                    std::span<const hog::DVec2> points) {
    using hog::terrain::fetch_headings;
    using hog::terrain::shore_size;
    using hog::terrain::shore_spacing;
    std::vector<float> depths;
    std::vector<float> still;
    std::vector<float> fetches;
    const auto each = [&](double spacing, const auto& at) {
        const double first = (0.5 - (shore_size / 2.0)) * spacing;
        for (int row = 0; row < shore_size; ++row) {
            for (int column = 0; column < shore_size; ++column) {
                at(hog::DVec2{first + (column * spacing), first + (row * spacing)});
            }
        }
    };
    for (const double spacing : shore_spacing) {
        each(spacing, [&](hog::DVec2 at) {
            depths.push_back(static_cast<float>(depth_at(at)));
            still.push_back(static_cast<float>(still_at(at)));
        });
        for (int heading = 0; heading < fetch_headings; ++heading) {
            each(spacing, [&](hog::DVec2 at) {
                fetches.push_back(static_cast<float>(fetch_at(at, heading)));
            });
        }
    }
    const auto layers = static_cast<std::uint32_t>(shore_spacing.size());
    auto image =
        hog::render::make_streamed_image(device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         {.width = shore_size, .height = shore_size}, 1, layers);
    REQUIRE(image);
    auto fetch_image = hog::render::make_streamed_image(device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                        {.width = shore_size, .height = shore_size},
                                                        1, layers * fetch_headings / 4);
    REQUIRE(fetch_image);
    const std::vector<std::uint16_t> texels = hog::render::shore_texels(depths, still);
    const std::vector<std::uint16_t> fetch_texels = hog::render::fetch_texels(fetches);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        REQUIRE(image->update(commands, 0, std::as_bytes(std::span{texels})));
        REQUIRE(fetch_image->update(commands, 0, std::as_bytes(std::span{fetch_texels})));
    }));
    auto sampler = hog::render::make_sampler(device, VK_FILTER_LINEAR);
    REQUIRE(sampler);

    const DVec3 eye = frame.anchor * (radius + 5.0);
    std::vector<glm::vec4> asked{
        glm::vec4{hog::Vec3{(frame.anchor * radius) - eye},
                  static_cast<float>(1.0 / shore_spacing[0])},
        glm::vec4{hog::Vec3{frame.east}, static_cast<float>(1.0 / shore_spacing[1])},
        glm::vec4{hog::Vec3{frame.north}, 0.0F}, glm::vec4{hog::Vec3{wind}, 0.0F}};
    for (const hog::DVec2 point : points) {
        const DVec3 rest = (hog::terrain::shore_direction(frame, radius, point) * radius) - eye;
        asked.emplace_back(hog::Vec3{rest}, static_cast<float>(radius));
    }
    constexpr auto binding = [](std::uint32_t at, VkDescriptorType type) {
        return VkDescriptorSetLayoutBinding{.binding = at,
                                            .descriptorType = type,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = nullptr};
    };
    const std::array bindings{binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                              binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                              binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                              binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};
    auto probe = hog::render::make_compute_pipeline(
        device, {.spirv = hog::shaders::shore_probe, .descriptors = bindings});
    REQUIRE(probe);
    auto questions = hog::render::make_filled_buffer(device, std::as_bytes(std::span{asked}),
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(questions);
    std::vector<std::array<float, 4>> found(points.size());
    auto answers =
        hog::render::make_buffer(device, std::span{found}.size_bytes(),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(answers);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe->handle());
        hog::render::push_sampled_image(commands, *probe, 0, image->image().view(),
                                        sampler->handle());
        hog::render::push_storage_buffer(commands, *probe, 1, questions->handle());
        hog::render::push_storage_buffer(commands, *probe, 2, answers->handle());
        hog::render::push_sampled_image(commands, *probe, 3, fetch_image->image().view(),
                                        sampler->handle());
        vkCmdDispatch(commands, static_cast<std::uint32_t>((points.size() + 63) / 64), 1, 1);
        const VkMemoryBarrier2 to_host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .pNext = nullptr,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                       .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .pNext = nullptr,
                                          .dependencyFlags = 0,
                                          .memoryBarrierCount = 1,
                                          .pMemoryBarriers = &to_host,
                                          .bufferMemoryBarrierCount = 0,
                                          .pBufferMemoryBarriers = nullptr,
                                          .imageMemoryBarrierCount = 0,
                                          .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(commands, &dependency);
    }));
    REQUIRE(answers->invalidate());
    std::memcpy(found.data(), answers->mapped().data(), std::span{found}.size_bytes());
    std::vector<ShoreFound> shore_found;
    shore_found.reserve(found.size());
    for (const auto& at : found) {
        shore_found.push_back({.depth = static_cast<double>(at[0]),
                               .still = static_cast<double>(at[1]),
                               .fetch = static_cast<double>(at[2])});
    }
    return shore_found;
}

} // namespace

TEST_CASE("the sea's shaders find the depths round the eye", "[render][ocean][gpu]") {
    const auto device = test_device();
    const hog::terrain::ShoreFrame frame =
        hog::terrain::shore_frame(glm::normalize(DVec3{0.2, -0.4, 0.9}), radius);
    // Rising to the east and falling to the north, which filtering keeps as it is.
    const auto depth_at = [](hog::DVec2 at) { return 20.0 + (0.004 * at.x) - (0.002 * at.y); };
    // Fine, where the fine grid fades into the coarse, coarse, and fading out of it.
    const std::array<hog::DVec2, 7> points{hog::DVec2{0.0, 0.0},      hog::DVec2{300.0, -420.0},
                                           hog::DVec2{-950.0, 100.0}, hog::DVec2{4000.0, -3000.0},
                                           hog::DVec2{7000.0, 0.0},   hog::DVec2{0.0, -9000.0},
                                           hog::DVec2{-20000.0, 5.0}};
    const std::vector<ShoreFound> found = probe_shore(
        *device, frame, depth_at, [](hog::DVec2) { return hog::terrain::shore_highest; },
        [](hog::DVec2, int) { return 0.0; }, frame.east, points);
    REQUIRE(found.size() == points.size());
    // Half floats keep about three digits.
    for (std::size_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        CHECK_THAT(found[i].depth, WithinAbs(depth_at(points[i]), 0.05));
    }
    const double inside = (1.0 - (7000.0 / 8192.0)) / 0.2;
    CHECK_THAT(found[4].depth, WithinAbs(1000.0 + ((depth_at(points[4]) - 1000.0) * inside), 1.0));
    CHECK(found[5].depth == 1000.0);
    CHECK(found[6].depth == 1000.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the sea's shaders find still water round the eye, and the fetch over it",
          "[render][ocean][gpu]") {
    const auto device = test_device();
    const hog::terrain::ShoreFrame frame =
        hog::terrain::shore_frame(glm::normalize(DVec3{-0.3, 0.5, 0.8}), radius);
    const auto still_at = [](hog::DVec2 at) { return 3.0 + (0.001 * at.x); };
    const auto fetch_at = [](hog::DVec2 at, int heading) {
        return (100.0 * (heading + 1)) + (0.05 * at.y);
    };
    const std::array<hog::DVec2, 3> points{hog::DVec2{0.0, 0.0}, hog::DVec2{300.0, -420.0},
                                           hog::DVec2{-2000.0, 3000.0}};
    // Blowing a third of the way from one heading to the next, and across east.
    const auto towards = [&](double degrees) {
        const double turned = degrees * std::numbers::pi / 180.0;
        return (frame.east * std::cos(turned)) + (frame.north * std::sin(turned));
    };
    for (const auto& [degrees, before, after] : {std::tuple{15.0, 0, 1}, std::tuple{330.0, 7, 0}}) {
        CAPTURE(degrees);
        const std::vector<ShoreFound> found = probe_shore(
            *device, frame, [](hog::DVec2) { return hog::terrain::shore_highest; }, still_at,
            fetch_at, towards(degrees), points);
        const double between = std::fmod(degrees, 45.0) / 45.0;
        for (std::size_t i = 0; i < points.size(); ++i) {
            CAPTURE(i);
            CHECK_THAT(found[i].still, WithinAbs(still_at(points[i]), 0.01));
            CHECK_THAT(found[i].fetch, WithinAbs(std::lerp(fetch_at(points[i], before),
                                                           fetch_at(points[i], after), between),
                                                 0.5));
        }
    }
    CHECK(device->validation_errors() == 0);
}
