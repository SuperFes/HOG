#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/terrain.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/material.hpp>
#include <hog/terrain/stitching.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::planet::Biome;
using hog::render::TerrainDraw;
using hog::render::testing::test_device;
using hog::terrain::patch_cells;
using hog::terrain::patch_side;
using hog::terrain::PatchVertex;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

constexpr VkExtent2D extent{.width = 64, .height = 64};
constexpr Pixel background{0, 0, 0, 255};

// Grey all over, but for grassland in green.
std::vector<hog::terrain::MaterialImage> plain_materials() {
    std::vector<hog::terrain::MaterialImage> materials;
    for (int biome = 0; biome < hog::planet::biome_count; ++biome) {
        const DVec3 colour =
            static_cast<Biome>(biome) == Biome::Grassland ? DVec3{0.0, 0.75, 0.25} : DVec3{0.5};
        materials.push_back(hog::terrain::material_image({.colour = colour}, 64));
    }
    return materials;
}

// Level ground facing +z, `spacing` metres between vertices about the patch's origin, its skirts
// `skirt` metres deep. Odd vertices along its -u edge can be pushed off it.
std::vector<PatchVertex> flat_patch(float spacing, hog::Vec3 odd_pushed = {}, float skirt = 0.0F) {
    constexpr int middle = patch_cells / 2;
    std::vector<PatchVertex> vertices;
    for (int j = 0; j < patch_side; ++j) {
        for (int i = 0; i < patch_side; ++i) {
            const hog::Vec3 pushed = i == 0 && j % 2 != 0 ? odd_pushed : hog::Vec3{};
            vertices.push_back(
                {.position = hog::Vec3{static_cast<float>(i - middle) * spacing,
                                       static_cast<float>(j - middle) * spacing, 0.0F} +
                             pushed,
                 .normal = {0.0F, 0.0F, 1.0F},
                 .biome = Biome::Grassland});
        }
    }
    for (int edge = 0; edge < 4; ++edge) {
        for (int along = 0; along < patch_side; ++along) {
            const hog::terrain::GridPoint above = hog::terrain::on_edge(edge, along);
            PatchVertex under = vertices.at((static_cast<std::size_t>(above.j) * patch_side) +
                                            static_cast<std::size_t>(above.i));
            under.position.z -= skirt;
            vertices.push_back(under);
        }
    }
    return vertices;
}

hog::render::TerrainPool make_pool(const hog::render::Device& device,
                                   hog::render::TerrainPoolDesc desc) {
    auto pool = hog::render::TerrainPool::create(device, desc);
    REQUIRE(pool);
    return std::move(pool).value();
}

// As plain_materials, but grassland's is grainy and mottled.
std::vector<hog::terrain::MaterialImage> grainy_materials() {
    std::vector<hog::terrain::MaterialImage> materials = plain_materials();
    materials.at(static_cast<std::size_t>(Biome::Grassland)) = hog::terrain::material_image(
        {.colour = {0.0, 0.75, 0.25}, .grain = 0.5, .mottle = 0.3, .seed = 1}, 64);
    return materials;
}

hog::render::Terrain
make_terrain(hog::render::Device& device,
             const std::vector<hog::terrain::MaterialImage>& materials = plain_materials()) {
    auto terrain = hog::render::Terrain::create(device, VK_FORMAT_R8G8B8A8_UNORM,
                                                VK_FORMAT_D32_SFLOAT, {.materials = materials});
    REQUIRE(terrain);
    return std::move(terrain).value();
}

// Looking straight down at `below` from `height` metres over it, with +y up the view.
hog::render::TerrainView looking_down(DVec3 below, double height, DVec3 sun = {0.0, 0.0, 1.0}) {
    return {.camera = {.eye = WorldPosition{below + DVec3{0.0, 0.0, height}},
                       .forward = {0.0, 0.0, -1.0},
                       .up = {0.0, 1.0, 0.0},
                       .vertical_fov = 1.0,
                       .near_plane = 0.1},
            .sun = sun,
            .night_light = 0.5};
}

// Uploads what's staged, then draws over a black background and reads it back.
std::vector<std::uint8_t> draw(hog::render::Device& device, const hog::render::Terrain& terrain,
                               hog::render::TerrainPool& pool, std::span<const TerrainDraw> draws,
                               const hog::render::TerrainView& view) {
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
        REQUIRE(pool.record_uploads(commands, 0));
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, target->view(), extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        terrain.draw(commands, extent, pool, draws, view);
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

bool near(Pixel a, Pixel b) {
    for (std::size_t channel = 0; channel < a.size(); ++channel) {
        if (std::abs(int{a.at(channel)} - int{b.at(channel)}) > 1) {
            return false;
        }
    }
    return true;
}

// The most any channel strays between the middle quarter's pixels.
int spread(const std::vector<std::uint8_t>& pixels) {
    int least = 255;
    int most = 0;
    for (std::uint32_t y = 3 * extent.height / 8; y < 5 * extent.height / 8; ++y) {
        for (std::uint32_t x = 3 * extent.width / 8; x < 5 * extent.width / 8; ++x) {
            const int green = pixel_at(pixels, x, y)[1];
            least = std::min(least, green);
            most = std::max(most, green);
        }
    }
    return most - least;
}

int background_pixels(const std::vector<std::uint8_t>& pixels) {
    int count = 0;
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            count += pixel_at(pixels, x, y) == background ? 1 : 0;
        }
    }
    return count;
}

// One patch two metres across, seen from four metres over its origin.
std::vector<std::uint8_t>
one_patch(hog::render::Device& device, DVec3 origin, DVec3 sun = {0.0, 0.0, 1.0},
          const std::vector<hog::terrain::MaterialImage>& materials = plain_materials()) {
    const auto terrain = make_terrain(device, materials);
    auto pool = make_pool(device, {});
    const std::uint32_t slot = pool.acquire().value_or(1);
    REQUIRE(slot == 0);
    REQUIRE(pool.stage(0, slot, flat_patch(2.0F / patch_cells)));
    const std::array draws{TerrainDraw{.slot = slot, .origin = WorldPosition{origin}}};
    return draw(device, terrain, pool, draws, looking_down(origin, 4.0, sun));
}

// The middle row of what's drawn over the background.
double middle_row(const std::vector<std::uint8_t>& pixels) {
    std::uint32_t top = extent.height;
    std::uint32_t bottom = 0;
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            if (pixel_at(pixels, x, y) != background) {
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    REQUIRE(top <= bottom);
    return (static_cast<double>(top + bottom) / 2.0) + 0.5;
}

// The row an offset from the eye is drawn at.
double row_of(const hog::render::Camera& camera, DVec3 from_eye) {
    const glm::vec4 clip =
        hog::render::view_projection(camera, 1.0) * glm::vec4{hog::Vec3{from_eye}, 1.0F};
    return (static_cast<double>(clip.y / clip.w) + 1.0) / 2.0 * extent.height;
}

} // namespace

TEST_CASE("a terrain pool hands out each slot once, and takes them back", "[render][gpu]") {
    const auto device = test_device();
    auto pool = make_pool(*device, {.patches = 3});
    CHECK(pool.capacity() == 3);
    CHECK(pool.acquire() == 0U);
    CHECK(pool.acquire() == 1U);
    CHECK(pool.acquire() == 2U);
    CHECK(!pool.acquire());
    pool.release(1);
    CHECK(pool.free_slots() == 1);
    CHECK(pool.acquire() == 1U);

    const hog::testing::AssertionGuard guard;
    pool.release(0);
    CHECK_THROWS_AS(pool.release(0), hog::testing::AssertionFailed);
}

TEST_CASE("terrain uploads stay within a frame's budget", "[render][gpu]") {
    const auto device = test_device();
    auto pool = make_pool(*device, {.patches = 4, .frames = 2, .uploads = 2});
    const auto patch = flat_patch(1.0F);
    for (int slot = 0; slot < 3; ++slot) {
        REQUIRE(pool.acquire());
    }
    CHECK(pool.stage(0, 0, patch));
    CHECK(pool.stage(0, 1, patch));
    // Staging a slot again replaces it rather than spending more.
    CHECK(pool.stage(0, 1, patch));
    CHECK(pool.stage(0, 2, patch).error().code == hog::ErrorCode::OutOfRange);
    // Each frame has its own budget, spent again once it's recorded.
    CHECK(pool.stage(1, 2, patch));
    REQUIRE(device->submit_now(
        [&](VkCommandBuffer commands) { REQUIRE(pool.record_uploads(commands, 0)); }));
    CHECK(pool.stage(0, 2, patch));

    CHECK(pool.stage(2, 0, patch).error().code == hog::ErrorCode::InvalidArgument);
    CHECK(pool.stage(0, 3, patch).error().code == hog::ErrorCode::InvalidArgument);
    CHECK(pool.stage(0, 0, std::span{patch}.first(10)).error().code ==
          hog::ErrorCode::InvalidArgument);
    CHECK(!hog::render::TerrainPool::create(*device, {.patches = 0}));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a patch is drawn where it lies from the eye, lit by the sun", "[render][gpu]") {
    const auto device = test_device();
    const auto lit = one_patch(*device, {});
    // Four metres up, the view is 4.4 m across and the patch two.
    CHECK(near(pixel_at(lit, 32, 32), {0, 191, 64, 255}));
    CHECK(near(pixel_at(lit, 32, 20), {0, 191, 64, 255}));
    CHECK(pixel_at(lit, 32, 4) == background);
    CHECK(pixel_at(lit, 4, 32) == background);

    // From behind, only the night light is left.
    const auto unlit = one_patch(*device, {}, {0.0, 0.0, -1.0});
    CHECK(near(pixel_at(unlit, 32, 32), {0, 96, 32, 255}));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("far from the planet's centre, patches are drawn as they are near it", "[render][gpu]") {
    const auto device = test_device();
    // A float at this distance is half a metre apart from the next.
    const DVec3 far{6.4e6, -3.2e6, 1.6e6};
    CHECK(one_patch(*device, far) == one_patch(*device, {}));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("patches meet across a level boundary without cracks", "[render][gpu]") {
    const auto device = test_device();
    const auto terrain = make_terrain(*device);
    auto pool = make_pool(*device, {.patches = 3, .uploads = 3});
    for (int slot = 0; slot < 3; ++slot) {
        REQUIRE(pool.acquire());
    }
    // A coarse patch to -x of two finer ones, whose odd vertices along the seam lie off it, as
    // nothing on the coarse side stands there to meet them.
    constexpr float fine = 1.0F;
    constexpr double half = patch_cells / 2.0;
    REQUIRE(pool.stage(0, 0, flat_patch(2.0F * fine)));
    REQUIRE(pool.stage(0, 1, flat_patch(fine, {fine / 2.0F, 0.0F, 0.0F})));
    REQUIRE(pool.stage(0, 2, flat_patch(fine, {fine / 2.0F, 0.0F, 0.0F})));
    const auto seam = [&](std::uint8_t fine_edges) {
        const std::array draws{
            TerrainDraw{.slot = 0, .origin = WorldPosition{DVec3{-2.0 * half, 0.0, 0.0}}},
            TerrainDraw{.slot = 1,
                        .origin = WorldPosition{DVec3{half, -half, 0.0}},
                        .coarser_edges = fine_edges},
            TerrainDraw{.slot = 2,
                        .origin = WorldPosition{DVec3{half, half, 0.0}},
                        .coarser_edges = fine_edges}};
        // Sixteen metres across, all of it over the patches, with the seam down the middle.
        return draw(*device, terrain, pool, draws, looking_down({}, 14.6));
    };
    CHECK(background_pixels(seam(hog::terrain::edge_minus_u)) == 0);
    CHECK(background_pixels(seam(0)) > 0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("skirts hide the gaps beside a neighbour left unstitched", "[render][gpu]") {
    const auto device = test_device();
    const auto terrain = make_terrain(*device);
    constexpr float fine = 1.0F;
    constexpr double half = patch_cells / 2.0;
    // As beside a stand-in levels coarser: the finer side's odd vertices along the seam stand
    // over the coarse side's straight edge, and nothing stitches them to it.
    const auto seam = [&](float skirt) {
        auto pool = make_pool(*device, {.patches = 3, .uploads = 3});
        for (int slot = 0; slot < 3; ++slot) {
            REQUIRE(pool.acquire());
        }
        REQUIRE(pool.stage(0, 0, flat_patch(2.0F * fine, {}, skirt)));
        REQUIRE(pool.stage(0, 1, flat_patch(fine, {0.0F, 0.0F, fine / 2.0F}, skirt)));
        REQUIRE(pool.stage(0, 2, flat_patch(fine, {0.0F, 0.0F, fine / 2.0F}, skirt)));
        const std::array draws{
            TerrainDraw{.slot = 0, .origin = WorldPosition{DVec3{-2.0 * half, 0.0, 0.0}}},
            TerrainDraw{.slot = 1, .origin = WorldPosition{DVec3{half, -half, 0.0}}},
            TerrainDraw{.slot = 2, .origin = WorldPosition{DVec3{half, half, 0.0}}}};
        // From over the coarse side, looking across the seam and down onto the finer side.
        const hog::render::TerrainView view{
            .camera = {.eye = WorldPosition{DVec3{-4.0, 0.0, 6.0}},
                       .forward = glm::normalize(DVec3{1.0, 0.0, -2.0}),
                       .up = glm::normalize(DVec3{2.0, 0.0, 1.0}),
                       .vertical_fov = 1.0,
                       .near_plane = 0.1},
            .sun = {0.0, 0.0, 1.0},
            .night_light = 0.5};
        return draw(*device, terrain, pool, draws, view);
    };
    CHECK(background_pixels(seam(0.0F)) > 0);
    CHECK(background_pixels(seam(4.0F * fine)) == 0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("bent, ground past the true horizon is drawn where the bend puts it", "[render][gpu]") {
    const auto device = test_device();
    const auto terrain = make_terrain(*device);
    constexpr double radius = 115'000.0;
    constexpr double eye_height = 3.0;
    constexpr double along = 6'000.0;
    // A wall 60 m square, standing on the ground `along` metres east and facing back west.
    std::vector<PatchVertex> wall = flat_patch(60.0F / patch_cells);
    for (PatchVertex& vertex : wall) {
        vertex.position = {0.0F, -vertex.position.x, vertex.position.y};
        vertex.normal = {-1.0F, 0.0F, 0.0F};
    }
    auto pool = make_pool(*device, {});
    REQUIRE(pool.acquire());
    REQUIRE(pool.stage(0, 0, wall));
    const DVec3 foot = radius * DVec3{std::sin(along / radius), 0.0, std::cos(along / radius)};
    const std::array draws{TerrainDraw{.slot = 0, .origin = WorldPosition{foot}}};

    // Looking east along the ground, 0.1 radians high.
    const WorldPosition eye{DVec3{0.0, 0.0, radius + eye_height}};
    hog::render::TerrainView view{.camera = {.eye = eye,
                                             .forward = {1.0, 0.0, 0.0},
                                             .up = {0.0, 0.0, 1.0},
                                             .vertical_fov = 0.1,
                                             .near_plane = 0.1},
                                  .sun = {-1.0, 0.0, 0.0},
                                  .night_light = 0.5};
    const DVec3 from_eye = foot - eye.metres();
    const double true_horizon =
        row_of(view.camera, {1.0, 0.0, -std::tan(std::acos(radius / (radius + eye_height)))});

    const double unbent = middle_row(draw(*device, terrain, pool, draws, view));
    CHECK(std::abs(unbent - row_of(view.camera, from_eye)) < 1.0);
    CHECK(unbent > true_horizon + 5.0);

    view.bend = hog::terrain::bend_from(eye, radius, 0.0);
    const double bent = middle_row(draw(*device, terrain, pool, draws, view));
    CHECK(std::abs(bent - row_of(view.camera, hog::terrain::bent(view.bend, from_eye))) < 1.0);
    CHECK(bent < true_horizon - 2.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("high up, the ground is drawn unbent", "[render][gpu]") {
    const auto device = test_device();
    const auto terrain = make_terrain(*device);
    constexpr double radius = 115'000.0;
    auto pool = make_pool(*device, {});
    REQUIRE(pool.acquire());
    REQUIRE(pool.stage(0, 0, flat_patch(2'000.0F / patch_cells)));
    // Off to one side of straight down, where bending would move it.
    const DVec3 below{0.0, 0.0, radius};
    const std::array draws{
        TerrainDraw{.slot = 0, .origin = WorldPosition{below + DVec3{4'000.0, 0.0, -70.0}}}};
    hog::render::TerrainView view = looking_down(below, 20'000.0);
    const auto unbent = draw(*device, terrain, pool, draws, view);
    view.bend = hog::terrain::bend_from(view.camera.eye, radius, 0.0);
    CHECK(view.bend.faked == radius);
    CHECK(draw(*device, terrain, pool, draws, view) == unbent);
    CHECK(background_pixels(unbent) > 0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("up close the ground shows its material, and from afar what it averages to",
          "[render][gpu]") {
    const auto device = test_device();
    const auto close = one_patch(*device, {}, {0.0, 0.0, 1.0}, grainy_materials());
    CHECK(spread(close) > 20);
    CHECK(spread(one_patch(*device, {})) == 0);
    // Wherever on the planet it's seen from.
    CHECK(one_patch(*device, {6.4e6, -3.2e6, 1.6e6}, {0.0, 0.0, 1.0}, grainy_materials()) == close);

    // Two kilometres across, from high enough that a pixel spans tens of tiles.
    const auto far = [&](const std::vector<hog::terrain::MaterialImage>& materials) {
        const auto terrain = make_terrain(*device, materials);
        auto pool = make_pool(*device, {});
        REQUIRE(pool.acquire());
        REQUIRE(pool.stage(0, 0, flat_patch(2'000.0F / patch_cells)));
        const std::array draws{TerrainDraw{.slot = 0, .origin = WorldPosition{}}};
        return pixel_at(draw(*device, terrain, pool, draws, looking_down({}, 3'000.0)), 32, 32);
    };
    const Pixel plain = far(plain_materials());
    const Pixel grainy = far(grainy_materials());
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CHECK(std::abs(int{grainy.at(channel)} - int{plain.at(channel)}) <= 3);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("biomes blend from one to the next across the ground between them", "[render][gpu]") {
    const auto device = test_device();
    const auto terrain = make_terrain(*device);
    // Grassland to -x of the middle, grey to +x.
    std::vector<PatchVertex> half_grass = flat_patch(2.0F / patch_cells);
    for (PatchVertex& vertex : half_grass) {
        vertex.biome = vertex.position.x < 0.0F ? Biome::Grassland : Biome::Desert;
    }
    auto pool = make_pool(*device, {});
    REQUIRE(pool.acquire());
    REQUIRE(pool.stage(0, 0, half_grass));
    const std::array draws{TerrainDraw{.slot = 0, .origin = WorldPosition{}}};
    // Close enough that a cell spans several pixels.
    const auto pixels = draw(*device, terrain, pool, draws, looking_down({}, 0.5));
    int previous = 0;
    int between = 0;
    for (std::uint32_t x = 0; x < extent.width; ++x) {
        const int red = pixel_at(pixels, x, extent.height / 2)[0];
        CHECK(red >= previous);
        previous = red;
        between += red > 5 && red < 120 ? 1 : 0;
    }
    CHECK(pixel_at(pixels, 0, 32)[0] == 0);
    CHECK(pixel_at(pixels, extent.width - 1, 32)[0] > 120);
    CHECK(between >= 3);
    CHECK(device->validation_errors() == 0);
}
