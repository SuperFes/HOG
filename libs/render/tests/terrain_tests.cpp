#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/terrain.hpp>
#include <hog/terrain/stitching.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
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

hog::render::BiomePalette palette() {
    hog::render::BiomePalette colours{};
    colours.fill({0.5F, 0.5F, 0.5F});
    colours.at(static_cast<std::size_t>(Biome::Grassland)) = {0.0F, 0.75F, 0.25F};
    return colours;
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

hog::render::Terrain make_terrain(const hog::render::Device& device) {
    auto terrain = hog::render::Terrain::create(device, VK_FORMAT_R8G8B8A8_UNORM,
                                                VK_FORMAT_D32_SFLOAT, palette());
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
std::vector<std::uint8_t> one_patch(hog::render::Device& device, DVec3 origin,
                                    DVec3 sun = {0.0, 0.0, 1.0}) {
    const auto terrain = make_terrain(device);
    auto pool = make_pool(device, {});
    const std::uint32_t slot = pool.acquire().value_or(1);
    REQUIRE(slot == 0);
    REQUIRE(pool.stage(0, slot, flat_patch(2.0F / patch_cells)));
    const std::array draws{TerrainDraw{.slot = slot, .origin = WorldPosition{origin}}};
    return draw(device, terrain, pool, draws, looking_down(origin, 4.0, sun));
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
