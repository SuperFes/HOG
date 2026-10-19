#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/planet/climate.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/forest.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/patch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::Vec3;
using hog::WorldPosition;
using hog::render::Forest;
using hog::render::ForestDraw;
using hog::render::GrassView;
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
    double spacing = 8.0;
    DVec3 offset{0.0};              // of the patch's origin, metres east and north of the up point
    std::uint32_t lattice_i = 2163; // deep in the stand field's woods
    std::uint32_t lattice_j = 56619;
    int shift = 2;
    int detail = 2;
    DVec3 wind{0.0};
    double seconds = 0.0;
    bool from_side = false; // low from south of the patch, facing the stand's front rank
};

// The patch's grown trees alone over black: from overhead, or from the side.
Pixels draw(hog::render::Device& device, const Patch& grown) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    std::array<Vec3, hog::planet::biome_count> palette{};
    palette.fill(Vec3{0.35F, 0.55F, 0.25F});
    auto forest = Forest::create(device, colour_format, depth_format, palette);
    REQUIRE(forest);
    auto pool = hog::render::TerrainPool::create(device, {.patches = 1, .frames = 1, .uploads = 1});
    REQUIRE(pool);
    const std::uint32_t slot = pool->acquire().value_or(99);
    REQUIRE(slot == 0);
    REQUIRE(pool->stage(0, slot, field(grown.biome, grown.spacing)));

    const WorldPosition origin{(up * (radius + ground_height)) + (east * grown.offset.x) +
                               (north * grown.offset.y)};
    const WorldPosition eye{grown.from_side
                                ? (up * (radius + ground_height + 6.0)) + (north * -80.0)
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
    const ForestDraw stand{.slot = slot,
                           .origin = origin,
                           .face = 0,
                           .lattice_i = grown.lattice_i,
                           .lattice_j = grown.lattice_j,
                           .shift = grown.shift,
                           .fade_far = 240.0,
                           .detail = grown.detail};
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
        forest->draw(commands, extent, *pool, {&stand, 1}, view);
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

std::size_t leafy_pixels(const Pixels& pixels) {
    std::size_t leafy = 0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
        const int red = pixels.at(4 * pixel);
        const int green = pixels.at((4 * pixel) + 1);
        leafy += (green > 20 && green > red) ? 1U : 0U;
    }
    return leafy;
}

// Placement is keyed to the lattice bit for bit; the shaded picture may still wobble by a
// single count where interpolation rounds at an edge.
bool nearly_same(const Pixels& a, const Pixels& b) {
    if (a.size() != b.size()) {
        return false;
    }
    std::size_t off = 0;
    for (std::size_t at = 0; at < a.size(); ++at) {
        const int apart = std::abs(int{a[at]} - int{b[at]});
        if (apart > 1) {
            return false;
        }
        off += apart == 0 ? 0U : 1U;
    }
    return off <= 16;
}

} // namespace

TEST_CASE("the grown forest stands where its biome grows it", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const std::size_t wooded = lit_pixels(draw(*device, {}));
    CHECK(wooded > 200);
    CHECK(lit_pixels(draw(*device, {.biome = hog::planet::Biome::Ocean})) == 0);
    // The same spot of the field grows nothing where the biome grows no trees.
    CHECK(lit_pixels(draw(*device, {.biome = hog::planet::Biome::Desert})) == 0);
}

TEST_CASE("a grown pine keeps its spot at every terrain level", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // As the stamps' test: the finer patch spans the coarser's south-west quarter, sharing its
    // lattice base a shift apart; the same pines must stand in the same spots either way.
    const Patch coarse{.spacing = 8.0, .offset = DVec3{64.0, 64.0, 0.0}, .shift = 2};
    const Patch fine{.spacing = 4.0, .offset = DVec3{0.0, 0.0, 0.0}, .shift = 3};
    const Pixels coarsely = draw(*device, coarse);
    const Pixels finely = draw(*device, fine);
    CHECK(lit_pixels(finely) > 50);
    CHECK(nearly_same(coarsely, finely));
}

TEST_CASE("grown pines carry trunks and needles", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const Pixels stand = draw(*device, {.shift = 1, .detail = 0, .from_side = true});
    CHECK(lit_pixels(stand) > 300);
    CHECK(barky_pixels(stand) > 10);
    CHECK(leafy_pixels(stand) > 100);
}

TEST_CASE("the baked details thin with distance, holding the stand", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const Pixels full = draw(*device, {.detail = 0, .from_side = true});
    const Pixels coarse = draw(*device, {.detail = 2, .from_side = true});
    const std::size_t fully = lit_pixels(full);
    const std::size_t coarsely = lit_pixels(coarse);
    CHECK(fully > 300);
    CHECK(coarsely > 300);
    CHECK(full != coarse);
}
