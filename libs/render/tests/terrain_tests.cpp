#include "probe.hpp"
#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/flora/season.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/terrain.hpp>
#include <hog/shaders/bend_probe.hpp>
#include <hog/shaders/projection_probe.hpp>
#include <hog/shaders/water_probe.hpp>
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
#include <cstring>
#include <functional>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::planet::Biome;
using hog::render::TerrainDraw;
using hog::render::testing::probed;
using hog::render::testing::Probed;
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

// Uploads what's staged, records `before` if given, then draws over a black background and reads
// it back.
std::vector<std::uint8_t> draw(hog::render::Device& device, const hog::render::Terrain& terrain,
                               hog::render::TerrainPool& pool, std::span<const TerrainDraw> draws,
                               const hog::render::TerrainView& view,
                               const std::function<void(VkCommandBuffer)>& before = {}) {
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
        if (before) {
            before(commands);
        }
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

constexpr double earth = 6'371'000.0;

// Grey all over.
std::vector<hog::terrain::MaterialImage> grey_materials() {
    return {static_cast<std::size_t>(hog::planet::biome_count),
            hog::terrain::material_image({.colour = DVec3{0.5}}, 64)};
}

DVec3 towards(double elevation) {
    const double radians = elevation * std::numbers::pi / 180.0;
    return {std::cos(radians), 0.0, std::sin(radians)};
}

// Grey ground on top of an Earth-sized planet seen straight down from `height`, lit through the
// air as if it faced `facing`, and the middle pixel's colour as the CPU model has it.
struct ThroughAir {
    Pixel drawn;
    DVec3 expected;
};

ThroughAir through_air(hog::render::Device& device, double height, DVec3 sun,
                       DVec3 facing = {0.0, 0.0, 1.0}) {
    constexpr double exposure = 10.0;
    auto terrain =
        hog::render::Terrain::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                                     {.materials = grey_materials(), .air = true});
    REQUIRE(terrain);
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(sky);
    auto pool = make_pool(device, {});
    const std::uint32_t slot = pool.acquire().value_or(1);
    std::vector<PatchVertex> patch = flat_patch(static_cast<float>(height) / patch_cells * 4.0F);
    for (PatchVertex& vertex : patch) {
        vertex.normal = hog::Vec3{facing};
    }
    REQUIRE(pool.stage(0, slot, patch));

    const DVec3 top{0.0, 0.0, earth};
    const hog::terrain::Bend bend{
        .up = {0.0, 0.0, 1.0}, .eye_height = height, .radius = earth, .faked = earth};
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), bend);
    const DVec3 sky_light = model.sky_irradiance(height * scale, sun);
    hog::render::TerrainView view = looking_down(top, height, sun);
    view.bend = bend;
    view.air = sky->bindings(0, *atmosphere);
    const hog::render::SkyView seen{.camera = view.camera,
                                    .sun = sun,
                                    .bend = bend,
                                    .exposure = exposure,
                                    .sky_light = sky_light};
    const std::array draws{TerrainDraw{.slot = slot, .origin = WorldPosition{top}}};
    const auto pixels = draw(device, *terrain, pool, draws, view, [&](VkCommandBuffer commands) {
        CHECK(sky->record_frame(commands, extent, 0, *atmosphere, seen));
    });

    const DVec3 on_ground =
        (model.transmittance(0.0, sun.z) * std::max(glm::dot(facing, sun), 0.0)) +
        (sky_light * (0.5 + (0.5 * facing.z)));
    const DVec3 radiance = 0.5 * on_ground / std::numbers::pi;
    const hog::atmosphere::Scattered between =
        model.march(height * scale, {0.0, 0.0, -1.0}, sun, height * scale);
    const double passed =
        (between.transmittance.r + between.transmittance.g + between.transmittance.b) / 3.0;
    const DVec3 seen_radiance = (radiance * passed) + between.light;
    const DVec3 expected = 255.0 * (DVec3{1.0} - glm::exp(-seen_radiance * exposure));
    return {.drawn = pixel_at(pixels, extent.width / 2, extent.height / 2), .expected = expected};
}

bool matches(const ThroughAir& seen, double counts) {
    for (glm::length_t channel = 0; channel < 3; ++channel) {
        if (std::abs(seen.drawn.at(static_cast<std::size_t>(channel)) - seen.expected[channel]) >
            counts) {
            return false;
        }
    }
    return true;
}

double blueness(Pixel pixel) {
    return static_cast<double>(pixel[2]) / std::max(1.0, static_cast<double>(pixel[0]));
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

TEST_CASE("lit through the air, ground takes the sun and the sky and is seen through the haze",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const ThroughAir near = through_air(*device, 200.0, towards(40.0));
    const ThroughAir far = through_air(*device, 20'000.0, towards(40.0));
    INFO("near " << int{near.drawn[0]} << ", " << int{near.drawn[1]} << ", " << int{near.drawn[2]}
                 << " against " << near.expected.r << ", " << near.expected.g << ", "
                 << near.expected.b);
    INFO("far " << int{far.drawn[0]} << ", " << int{far.drawn[1]} << ", " << int{far.drawn[2]}
                << " against " << far.expected.r << ", " << far.expected.g << ", "
                << far.expected.b);
    CHECK(matches(near, 4.0));
    CHECK(matches(far, 6.0));
    // Seen through more air, it's bluer.
    CHECK(blueness(far.drawn) > blueness(near.drawn));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("ground facing the setting sun is lit red", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const ThroughAir noon = through_air(*device, 200.0, towards(60.0), towards(60.0));
    const ThroughAir setting = through_air(*device, 200.0, towards(3.0), towards(3.0));
    INFO("noon " << int{noon.drawn[0]} << ", " << int{noon.drawn[1]} << ", " << int{noon.drawn[2]});
    INFO("setting " << int{setting.drawn[0]} << ", " << int{setting.drawn[1]} << ", "
                    << int{setting.drawn[2]});
    CHECK(matches(setting, 4.0));
    CHECK(blueness(noon.drawn) > 0.8);
    CHECK(blueness(setting.drawn) < 0.7);
    CHECK(device->validation_errors() == 0);
}

namespace {

// The plane shares ground facing `normal` takes where the planet's up is `up`, a case each.
std::vector<DVec3> plane_shares(hog::render::Device& device,
                                std::span<const std::pair<DVec3, DVec3>> cases) {
    std::vector<Probed> asked;
    for (const auto& [up, normal] : cases) {
        const glm::vec3 u{glm::normalize(up)};
        const glm::vec3 n{glm::normalize(normal)};
        asked.push_back({u.x, u.y, u.z, 0.0F});
        asked.push_back({n.x, n.y, n.z, 0.0F});
    }
    std::vector<DVec3> shares;
    shares.reserve(cases.size());
    for (const Probed& share :
         probed(device, hog::shaders::projection_probe, asked, cases.size())) {
        shares.emplace_back(share[0], share[1], share[2]);
    }
    return shares;
}

// Tilted from `up` by `degrees` towards `towards`.
DVec3 tilted(DVec3 up, DVec3 towards, double degrees) {
    const DVec3 across = glm::normalize(towards - (up * glm::dot(towards, up)));
    const double angle = degrees * std::numbers::pi / 180.0;
    return (up * std::cos(angle)) + (across * std::sin(angle));
}

int planes_taken(DVec3 shares) {
    return (shares.x > 0.0 ? 1 : 0) + (shares.y > 0.0 ? 1 : 0) + (shares.z > 0.0 ? 1 : 0);
}

} // namespace

TEST_CASE("flat ground away from where the planes meet takes a single projection, by the up",
          "[render][gpu]") {
    const auto device = test_device();
    // Up nearest x, then y, then z, each well away from the others; flat or gently sloping
    // ground, sloping away from the axis too.
    const std::array ups{DVec3{0.8, 0.5, 0.33}, DVec3{-0.4, -0.85, 0.35}, DVec3{0.3, 0.55, 0.78}};
    std::vector<std::pair<DVec3, DVec3>> cases;
    for (const DVec3& up : ups) {
        const DVec3 unit = glm::normalize(up);
        for (const double slope : {0.0, 10.0, 18.0}) {
            cases.emplace_back(unit, tilted(unit, DVec3{1.0, 1.0, 1.0} - glm::abs(unit), slope));
        }
    }
    const std::vector<DVec3> shares = plane_shares(*device, cases);
    for (std::size_t at = 0; at < cases.size(); ++at) {
        const DVec3 up = glm::abs(cases[at].first);
        glm::length_t axis = 2;
        if (up.x > up.y && up.x > up.z) {
            axis = 0;
        } else if (up.y > up.z) {
            axis = 1;
        }
        INFO("case " << at << ": " << shares[at].x << ", " << shares[at].y << ", " << shares[at].z);
        CHECK(shares[at][axis] == 1.0);
        CHECK(planes_taken(shares[at]) == 1);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("where the planes meet, flat ground's shares blend continuously", "[render][gpu]") {
    const auto device = test_device();
    // Up swept from nearest x to nearest y, across where they meet.
    std::vector<std::pair<DVec3, DVec3>> cases;
    constexpr int steps = 400;
    for (int step = 0; step <= steps; ++step) {
        const double angle = (0.15 + (0.5 * step / steps)) * std::numbers::pi / 2.0;
        const DVec3 up = glm::normalize(DVec3{std::cos(angle), std::sin(angle), 0.3});
        cases.emplace_back(up, up);
    }
    const std::vector<DVec3> shares = plane_shares(*device, cases);
    CHECK(shares.front() == DVec3{1.0, 0.0, 0.0});
    CHECK(shares.back() == DVec3{0.0, 1.0, 0.0});
    int blended = 0;
    for (std::size_t at = 0; at < shares.size(); ++at) {
        INFO("step " << at << ": " << shares[at].x << ", " << shares[at].y << ", " << shares[at].z);
        CHECK(std::abs(shares[at].x + shares[at].y + shares[at].z - 1.0) < 1e-5);
        CHECK(shares[at].z == 0.0);
        blended += planes_taken(shares[at]) == 2 ? 1 : 0;
        if (at > 0) {
            CHECK(glm::length(shares[at] - shares[at - 1]) < 0.15);
        }
    }
    // A narrow band, but a band.
    CHECK(blended > 2);
    CHECK(blended < steps / 10);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("steep ground follows its normal", "[render][gpu]") {
    const auto device = test_device();
    const DVec3 up{0.0, 0.0, 1.0};
    const std::array<std::pair<DVec3, DVec3>, 3> cases{
        {{up, DVec3{1.0, 0.0, 0.0}},
         {up, tilted(up, DVec3{0.0, 1.0, 0.0}, 60.0)},
         {glm::normalize(DVec3{0.8, 0.5, 0.33}), DVec3{0.0, 0.0, -1.0} + DVec3{0.1, 0.0, 0.0}}}};
    const std::vector<DVec3> shares = plane_shares(*device, cases);
    CHECK(shares[0] == DVec3{1.0, 0.0, 0.0});
    CHECK(shares[1] == DVec3{0.0, 1.0, 0.0});
    CHECK(shares[2] == DVec3{0.0, 0.0, 1.0});
    CHECK(device->validation_errors() == 0);
}

namespace {

// Woods a temperate stand might hold: about two thirds of its cells grown, 15 metres tall, half
// broadleaves and of those a third maples.
constexpr hog::terrain::Woods temperate{80, 120, 128, 85};

// Grey ground on top of a small planet seen straight down from `height` metres, its west half
// wooded with `woods` and its east bare, the woods drawn in beyond `canopy` metres, none for 0.
struct Seen {
    hog::terrain::Woods woods = temperate;
    std::uint8_t scar = 0; // of the west half, as terrain::Cover's
    double height = 3'000.0;
    double canopy = 900.0;
    bool air = false;
    hog::flora::Daylight daylight{};
    Biome biome = Biome::Grassland;
    double wind = 0.0; // m/s near the ground, lit through the air
    DVec3 gusts_drifted{0.0};
};

std::vector<std::uint8_t> from_above(hog::render::Device& device, const Seen& seen) {
    constexpr double small_planet = 115'000.0;
    auto terrain = hog::render::Terrain::create(
        device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
        {.materials = grey_materials(), .air = seen.air, .canopy = seen.canopy});
    REQUIRE(terrain);
    auto pool = make_pool(device, {});
    const std::uint32_t slot = pool.acquire().value_or(1);
    // Wide enough to fill the view.
    std::vector<PatchVertex> patch =
        flat_patch(static_cast<float>(seen.height) * 1.5F / patch_cells);
    for (PatchVertex& vertex : patch) {
        vertex.woods = vertex.position.x < 0.0F ? seen.woods : hog::terrain::Woods{};
        vertex.cover = {vertex.position.x < 0.0F ? seen.scar : std::uint8_t{0}, 0};
        vertex.biome = seen.biome;
    }
    REQUIRE(pool.stage(0, slot, patch));

    const DVec3 top{0.0, 0.0, small_planet};
    const DVec3 sun{0.0, 0.0, 1.0};
    hog::render::TerrainView view = looking_down(top, seen.height, sun);
    view.bend = {.up = {0.0, 0.0, 1.0},
                 .eye_height = seen.height,
                 .radius = small_planet,
                 .faked = small_planet};
    const std::array draws{TerrainDraw{.slot = slot, .origin = WorldPosition{top}}};
    if (!seen.air) {
        return draw(device, *terrain, pool, draws, view);
    }
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(sky);
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), view.bend);
    view.air = sky->bindings(0, *atmosphere);
    const hog::render::SkyView sky_view{.camera = view.camera,
                                        .sun = sun,
                                        .bend = view.bend,
                                        .exposure = 10.0,
                                        .sky_light = model.sky_irradiance(seen.height * scale, sun),
                                        .daylight = seen.daylight,
                                        .gusts_drifted = seen.gusts_drifted,
                                        .gusts_wind = seen.wind};
    return draw(device, *terrain, pool, draws, view, [&](VkCommandBuffer commands) {
        CHECK(sky->record_frame(commands, extent, 0, *atmosphere, sky_view));
    });
}

// A channel's mean over the west or east quarter of the view, away from where the woods end.
double mean_of(const std::vector<std::uint8_t>& pixels, std::size_t channel, bool west) {
    double sum = 0.0;
    int counted = 0;
    const std::uint32_t from = west ? 0 : 3 * extent.width / 4;
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = from; x < from + (extent.width / 4); ++x) {
            sum += pixel_at(pixels, x, y).at(channel);
            ++counted;
        }
    }
    return sum / counted;
}

// How far apart two pictures' channels lie on the whole, and at most.
struct Apart {
    double mean = 0.0;
    int most = 0;
};

Apart apart(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    Apart found;
    for (std::size_t at = 0; at < a.size(); ++at) {
        const int by = std::abs(int{a[at]} - int{b[at]});
        found.mean += by;
        found.most = std::max(found.most, by);
    }
    found.mean /= static_cast<double>(a.size());
    return found;
}

} // namespace

TEST_CASE("ground a fire left black is drawn charred, and only it", "[render][gpu]") {
    const auto device = test_device();
    const auto bare = from_above(*device, {.woods = {}, .canopy = 0.0});
    const auto black = from_above(*device, {.woods = {}, .scar = 255, .canopy = 0.0});
    const auto greening = from_above(*device, {.woods = {}, .scar = 100, .canopy = 0.0});
    INFO("west red " << mean_of(bare, 0, true) << ", black " << mean_of(black, 0, true)
                     << ", greening " << mean_of(greening, 0, true));
    CHECK(mean_of(black, 0, true) < 0.3 * mean_of(bare, 0, true));
    CHECK(mean_of(greening, 0, true) > mean_of(black, 0, true));
    CHECK(mean_of(greening, 0, true) < mean_of(bare, 0, true));
    CHECK(mean_of(black, 0, false) == mean_of(bare, 0, false));
}

TEST_CASE("woods beyond the stamps darken the ground they stand on, and only it", "[render][gpu]") {
    const auto device = test_device();
    const auto none = from_above(*device, {.canopy = 0.0});
    const auto woods = from_above(*device, {});
    INFO("west green " << mean_of(woods, 1, true) << " against " << mean_of(none, 1, true));
    CHECK(mean_of(woods, 1, true) < mean_of(none, 1, true) - 20.0);
    CHECK(mean_of(woods, 1, false) == mean_of(none, 1, false));

    // Thinner woods hide less of it, and none hide none.
    const auto thin = from_above(*device, {.woods = {20, 120, 128, 85}});
    CHECK(mean_of(thin, 1, true) > mean_of(woods, 1, true) + 5.0);
    CHECK(mean_of(thin, 1, true) < mean_of(none, 1, true));
    CHECK(from_above(*device, {.woods = {}}) == none);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("gusts cross far meadows as faint patches of light, and leave bare ground be",
          "[render][gpu]") {
    const auto device = test_device();
    const Seen still{.woods = {}, .height = 300.0, .canopy = 0.0, .air = true};
    Seen blowing = still;
    blowing.wind = 6.0;
    const auto before = from_above(*device, blowing);
    blowing.gusts_drifted = DVec3{40.0, -25.0, 10.0};
    const auto after = from_above(*device, blowing);
    const Apart drifted = apart(before, after);
    const Apart lit = apart(before, from_above(*device, still));
    INFO("drifting apart " << drifted.mean << " on the whole, " << drifted.most
                           << " at most; from still air " << lit.mean << ", " << lit.most);
    CHECK(drifted.mean > 1.5);
    CHECK(drifted.most < 16);
    CHECK(lit.most < 16);

    Seen calm = still;
    calm.gusts_drifted = blowing.gusts_drifted;
    CHECK(from_above(*device, calm) == from_above(*device, still));
    Seen beach = still;
    beach.biome = Biome::Beach;
    const auto bare = from_above(*device, beach);
    beach.wind = 6.0;
    beach.gusts_drifted = blowing.gusts_drifted;
    CHECK(from_above(*device, beach) == bare);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("within the stamps' reach the ground is left bare for them", "[render][gpu]") {
    const auto device = test_device();
    CHECK(from_above(*device, {.canopy = 10'000.0}) == from_above(*device, {.canopy = 0.0}));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("woods far off turn with the year, the pines' all year green", "[render][gpu]") {
    const auto device = test_device();
    // Earth's days at 45 degrees north: a July day, one in late October, and January's.
    constexpr hog::flora::Daylight summer{.hours = 15.0F, .longest = 15.4F};
    constexpr hog::flora::Daylight autumn{.hours = 10.7F, .longest = 15.4F};
    constexpr hog::flora::Daylight winter{.hours = 9.0F, .longest = 15.4F, .lengthening = true};
    // Broadleaves alone, seen through less of the air's haze.
    constexpr hog::terrain::Woods broadleaves{80, 120, 255, 85};
    const auto seen_on = [&](hog::flora::Daylight day) {
        return from_above(*device,
                          {.woods = broadleaves, .height = 1'500.0, .air = true, .daylight = day});
    };
    const auto green = seen_on(summer);
    const auto turned = seen_on(autumn);
    const auto bare = seen_on(winter);
    // What the woods take from the ground's red against its green, the haze over both alike.
    const auto none = from_above(*device, {.height = 1'500.0, .canopy = 0.0, .air = true});
    const auto redness = [&](const std::vector<std::uint8_t>& pixels) {
        return (mean_of(none, 0, true) - mean_of(pixels, 0, true)) /
               std::max(mean_of(none, 1, true) - mean_of(pixels, 1, true), 1.0);
    };
    INFO("red to green taken: summer " << redness(green) << ", autumn " << redness(turned));
    CHECK(redness(turned) < redness(green) * 0.92);
    // Bare, the woods hide less of the ground.
    INFO("green: summer " << mean_of(green, 1, true) << ", winter " << mean_of(bare, 1, true));
    CHECK(mean_of(bare, 1, true) > mean_of(green, 1, true) + 2.0);

    constexpr hog::terrain::Woods pines{80, 120, 0, 0};
    CHECK(from_above(*device, {.woods = pines, .air = true, .daylight = winter}) ==
          from_above(*device, {.woods = pines, .air = true, .daylight = summer}));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("palm groves far off are woods of their own, green the year round", "[render][gpu]") {
    const auto device = test_device();
    constexpr hog::flora::Daylight summer{.hours = 15.0F, .longest = 15.4F};
    constexpr hog::flora::Daylight winter{.hours = 9.0F, .longest = 15.4F, .lengthening = true};
    constexpr hog::terrain::Woods palms{80, 120, 0, 0, 255};
    constexpr hog::terrain::Woods pines{80, 120, 0, 0, 0};
    const auto summer_palms =
        from_above(*device, {.woods = palms, .air = true, .daylight = summer});
    CHECK(summer_palms == from_above(*device, {.woods = palms, .air = true, .daylight = winter}));
    CHECK(summer_palms != from_above(*device, {.woods = pines, .air = true, .daylight = summer}));
    const auto none = from_above(*device, {.canopy = 0.0, .air = true});
    CHECK(mean_of(summer_palms, 1, true) < mean_of(none, 1, true) - 10.0);
    CHECK(device->validation_errors() == 0);
}

namespace {

struct BendProbed {
    DVec3 bent;
    double sine = 0.0;
    double arctangent = 0.0;
};

// Each point from the eye bent as `bend` has it, and the sine of each angle and the arctangent of
// each y over x.
std::vector<BendProbed> bend_probed(hog::render::Device& device, const hog::terrain::Bend& bend,
                                    std::span<const DVec3> points, std::span<const double> angles,
                                    std::span<const std::pair<double, double>> over) {
    std::vector<Probed> asked;
    for (std::size_t at = 0; at < points.size(); ++at) {
        const glm::vec3 point{points[at]};
        const glm::vec3 up{bend.up};
        asked.push_back({point.x, point.y, point.z, static_cast<float>(angles[at])});
        asked.push_back({up.x, up.y, up.z, static_cast<float>(bend.eye_height)});
        asked.push_back({static_cast<float>(bend.radius), static_cast<float>(bend.faked),
                         static_cast<float>(over[at].first), static_cast<float>(over[at].second)});
    }
    const std::vector<Probed> found =
        probed(device, hog::shaders::bend_probe, asked, 2 * points.size());
    std::vector<BendProbed> results;
    for (std::size_t at = 0; at < points.size(); ++at) {
        const Probed& point = found[2 * at];
        const Probed& more = found[(2 * at) + 1];
        results.push_back({.bent = DVec3{glm::vec3{point[0], point[1], point[2]}},
                           .sine = static_cast<double>(more[0]),
                           .arctangent = static_cast<double>(more[1])});
    }
    return results;
}

double as_float(double value) {
    return static_cast<double>(static_cast<float>(value));
}

} // namespace

TEST_CASE("bent, ground near the eye is drawn within a millimetre of where the bend puts it",
          "[render][gpu]") {
    const auto device = test_device();
    const hog::terrain::Bend bend{.up = glm::normalize(DVec3{0.3, -0.5, 0.8}),
                                  .eye_height = 2.0,
                                  .radius = 115'000.0,
                                  .faked = 6'371'000.0};
    const DVec3 across = glm::normalize(glm::cross(bend.up, DVec3{1.0, 0.0, 0.0}));
    std::vector<DVec3> points;
    // From half a metre out to five kilometres.
    for (int step = 0; step < 36; ++step) {
        const double out = 0.5 * std::pow(1.3, step);
        points.emplace_back(glm::vec3{(across * out) - (bend.up * 1.5)});
    }
    const std::vector<double> angles(points.size(), 0.0);
    const std::vector<std::pair<double, double>> over(points.size(), {0.0, 1.0});
    const std::vector<BendProbed> found = bend_probed(*device, bend, points, angles, over);
    for (std::size_t at = 0; at < points.size(); ++at) {
        const DVec3 expected = hog::terrain::bent(bend, points[at]);
        INFO("out " << glm::length(points[at]) << ": off by "
                    << glm::length(found[at].bent - expected));
        CHECK(glm::length(found[at].bent - expected) < 1e-3 + (1e-6 * glm::length(points[at])));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the bend's sines and arctangents of small angles are as precise as of large ones",
          "[render][gpu]") {
    const auto device = test_device();
    std::vector<double> angles;
    std::vector<std::pair<double, double>> over;
    // From a tenth of a microradian to past where the series give way.
    for (int step = 0; step < 39; ++step) {
        const double small = 1e-7 * std::pow(1.5, step);
        for (const double sign : {1.0, -1.0}) {
            angles.push_back(as_float(sign * small));
            over.emplace_back(as_float(sign * small * 0.4 * 3'000.0), 3'000.0);
        }
    }
    const std::vector<DVec3> points(angles.size(), DVec3{0.0});
    const std::vector<BendProbed> found =
        bend_probed(*device, hog::terrain::Bend{}, points, angles, over);
    for (std::size_t at = 0; at < angles.size(); ++at) {
        const double sine = std::sin(angles[at]);
        const double arctangent = std::atan2(over[at].first, over[at].second);
        INFO("angle " << angles[at] << ": sine " << found[at].sine << " for " << sine
                      << ", arctangent " << found[at].arctangent << " for " << arctangent);
        CHECK(std::abs(found[at].sine - sine) <= 1e-6 * std::abs(sine));
        CHECK(std::abs(found[at].arctangent - arctangent) <= 1e-6 * std::abs(arctangent));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("waves bend what's seen through the sea's surface by how they tilt it, as refracted",
          "[render][gpu]") {
    const auto device = test_device();
    constexpr DVec3 level{0.0, 0.0, 1.0};
    constexpr double into_water = 1.0 / 1.333;
    // Down and ahead, along x, steeply and at a glance.
    const std::array<DVec3, 2> lookings{glm::normalize(DVec3{1.0, 0.0, -2.0}),
                                        glm::normalize(DVec3{1.0, 0.0, -0.2})};
    // Level, tilted ahead, back and aside.
    const std::array<DVec3, 4> normals{level, glm::normalize(DVec3{0.2, 0.0, 1.0}),
                                       glm::normalize(DVec3{-0.2, 0.0, 1.0}),
                                       glm::normalize(DVec3{0.0, 0.2, 1.0})};
    constexpr std::array unders{1.0, 2.0};
    std::vector<Probed> asked;
    std::vector<DVec3> wanted;
    for (const DVec3& looking : lookings) {
        for (const DVec3& normal : normals) {
            for (const double under : unders) {
                asked.push_back({static_cast<float>(looking.x), static_cast<float>(looking.y),
                                 static_cast<float>(looking.z), 0.0F});
                asked.push_back({static_cast<float>(normal.x), static_cast<float>(normal.y),
                                 static_cast<float>(normal.z), 0.0F});
                asked.push_back({static_cast<float>(level.x), static_cast<float>(level.y),
                                 static_cast<float>(level.z), static_cast<float>(under)});
                wanted.push_back((glm::refract(looking, normal, into_water) -
                                  glm::refract(looking, level, into_water)) *
                                 under);
            }
        }
    }
    const std::vector<Probed> found =
        probed(*device, hog::shaders::water_probe, asked, wanted.size());
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        CAPTURE(i);
        const DVec3 moved{found[i][0], found[i][1], found[i][2]};
        CHECK(glm::length(moved - wanted[i]) < 1e-4);
    }
    for (std::size_t i = 0; i < wanted.size(); i += 2) {
        const std::size_t normal = (i / 2) % normals.size();
        CAPTURE(i);
        // Still water bends nothing; the further beyond the surface, the further it's moved.
        if (normal == 0) {
            CHECK(glm::length(wanted[i]) == 0.0);
        } else {
            CHECK(glm::length(wanted[i + 1] - (2.0 * wanted[i])) < 1e-9);
        }
        // Into the water the line of sight bends towards the normal's underside, against the way
        // the surface tilts.
        if (normal == 1) {
            CHECK(wanted[i].x < 0.0);
        }
        if (normal == 2) {
            CHECK(wanted[i].x > 0.0);
        }
        if (normal == 3) {
            CHECK(wanted[i].y < 0.0);
        }
    }
    CHECK(device->validation_errors() == 0);
}
