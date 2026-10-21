#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/terrain.hpp>
#include <hog/shaders/waves_probe.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/material.hpp>
#include <hog/terrain/stitching.hpp>
#include <hog/waves/waves.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <optional>
#include <random>
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
// metres each, and how the surface faces there; and how rough what's left is.
struct Probed {
    DVec3 moved;
    DVec3 normal;
    double roughness;
};

std::vector<Probed> probe_waves(hog::render::Device& device, const hog::render::DrawnWaves& drawn,
                                std::span<const DVec3> rest, double footprint) {
    std::vector<std::array<float, 4>> asked;
    for (const DVec3& point : rest) {
        asked.push_back({static_cast<float>(point.x), static_cast<float>(point.y),
                         static_cast<float>(point.z), static_cast<float>(footprint)});
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
    std::vector<std::array<float, 4>> found(2 * asked.size());
    auto answers =
        hog::render::make_buffer(device, std::span{found}.size_bytes(),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(answers);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe->handle());
        hog::render::push_uniform_buffer(commands, *probe, 0, waves->handle());
        hog::render::push_storage_buffer(commands, *probe, 1, questions->handle());
        hog::render::push_storage_buffer(commands, *probe, 2, answers->handle());
        vkCmdDispatch(commands, static_cast<std::uint32_t>((asked.size() + 63) / 64), 1, 1);
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
        const auto& moved = found.at(2 * i);
        const auto& normal = found.at((2 * i) + 1);
        probed.push_back({.moved = DVec3{hog::Vec3{moved[0], moved[1], moved[2]}},
                          .normal = DVec3{hog::Vec3{normal[0], normal[1], normal[2]}},
                          .roughness = static_cast<double>(moved[3])});
    }
    return probed;
}

// Eight metres over a sea raised by a wind of `wind` m/s, somewhere off the top of the planet.
struct WavySea {
    std::vector<hog::waves::Train> trains = hog::waves::planet_trains(5);
    WorldPosition eye{glm::normalize(DVec3{0.03, -0.02, 1.0}) * (radius + 8.0)};
    hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, 0.0);
    std::vector<hog::waves::Wave> waves;
    hog::render::DrawnWaves drawn;

    explicit WavySea(double wind) {
        const DVec3 along = glm::normalize(glm::cross(bend.up, north));
        waves = hog::waves::waves_at(
            trains, bend.up,
            {.systems = {hog::waves::grown_sea(along * wind)}, .current = {}, .wind = {}});
        drawn = hog::render::drawn_waves_at(trains, waves, eye, bend, noon);
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

TEST_CASE("waves too short to see from afar fade into the sea's roughness", "[render][gpu]") {
    const auto device = test_device();
    const WavySea sea{9.0};
    const std::vector<DVec3> rest{sea.rest(DVec3{40.0, 0.0, 0.0})};
    const Probed near = probe_waves(*device, sea.drawn, rest, 1e-3).front();
    const Probed far = probe_waves(*device, sea.drawn, rest, 5.0).front();
    const Probed farther = probe_waves(*device, sea.drawn, rest, 1e4).front();
    const auto all = static_cast<double>(sea.drawn.count_left_radius.y +
                                         sea.drawn.waves.front().amplitude_phase_slope_length.z);
    CHECK(near.roughness < far.roughness);
    CHECK(far.roughness < farther.roughness);
    // Seen from afar all of it is roughness, and what showed of the shape is gone.
    CHECK(std::abs(farther.roughness - all) < all * 1e-3);
    CHECK(glm::length(farther.moved) == 0.0);
    CHECK(glm::length(far.moved) > 0.0);
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
    view.waves = hog::render::drawn_waves_at(trains, hog::waves::waves_at(trains, up, windy),
                                             view.camera.eye, view.bend, noon);
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
