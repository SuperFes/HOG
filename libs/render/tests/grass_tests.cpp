#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/climate.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/grass.hpp>
#include <hog/render/grass_lattice.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/patch.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::Vec3;
using hog::WorldPosition;
using hog::planet::FaceIJ;
using hog::render::Grass;
using hog::render::GrassDraw;
using hog::render::GrassView;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 96, .height = 54};
constexpr VkFormat colour_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr double radius = 115'000.0;
constexpr double ground_height = 0.5; // above sea level, clear of the shoreline
constexpr double spacing = 0.5;       // metres between the field's vertices
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};

using Pixels = std::vector<std::uint8_t>;

// The world the test fields grow on, and the cell of a patch `spacing` across, near enough.
constexpr hog::render::GrassLattice lattice{.level = 21, .seed = 0x5eedU};
constexpr FaceIJ field_cell{.face = 0, .i = 4'100, .j = 4'100, .level = 13};

// A flat patch of one biome, or two blended, level at `ground_height`, centred on its origin, i
// running east and j north.
std::vector<hog::terrain::PatchVertex>
field(hog::planet::Biome biome, hog::terrain::Cover cover = {},
      hog::planet::Biome blend_biome = hog::planet::Biome::Ocean, std::uint8_t blend = 0,
      double across = spacing) {
    std::vector<hog::terrain::PatchVertex> vertices(hog::render::patch_vertex_count);
    constexpr int middle = hog::terrain::patch_cells / 2;
    for (int j = 0; j < hog::terrain::patch_side; ++j) {
        for (int i = 0; i < hog::terrain::patch_side; ++i) {
            const std::size_t at = (static_cast<std::size_t>(j) * hog::terrain::patch_side) +
                                   static_cast<std::size_t>(i);
            vertices[at] = {.position = Vec3{static_cast<float>((i - middle) * across),
                                             static_cast<float>((j - middle) * across), 0.0F},
                            .normal = Vec3{0.0F, 0.0F, 1.0F},
                            .biome = biome,
                            .cover = cover,
                            .blend_biome = blend_biome,
                            .blend = blend};
        }
    }
    return vertices;
}

struct Field {
    hog::planet::Biome biome = hog::planet::Biome::Grassland;
    DVec3 wind{0.0};
    double seconds = 0.0;
    DVec3 gusts_drifted{0.0};
    double away = 0.0; // metres east of the eye to the patch's centre
    hog::render::GrassTier grass{};
    double ground = ground_height; // metres above sea level
    hog::terrain::Cover cover{};
    hog::planet::Biome blend_biome = hog::planet::Biome::Ocean;
    std::uint8_t blend = 0;
    VkExtent2D size = extent;
    bool looking_away = false; // west, with the field behind the eye
};

// The field's grass alone over black, seen from standing height looking slightly down, east;
// and the blades grown, where asked.
Pixels draw(hog::render::Device& device, const Field& grown, std::uint32_t* counted = nullptr) {
    const VkExtent2D frame = grown.size;
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    std::array<Vec3, hog::planet::biome_count> palette{};
    palette.fill(Vec3{0.35F, 0.55F, 0.25F});
    auto grass = Grass::create(device, colour_format, depth_format, palette, lattice, grown.grass);
    REQUIRE(grass);
    auto pool = hog::render::TerrainPool::create(device, {.patches = 1, .frames = 1, .uploads = 1});
    REQUIRE(pool);
    const std::uint32_t slot = pool->acquire().value_or(99);
    REQUIRE(slot == 0);
    REQUIRE(pool->stage(0, slot, field(grown.biome, grown.cover, grown.blend_biome, grown.blend)));

    const WorldPosition origin{(up * (radius + grown.ground)) + (east * grown.away)};
    const WorldPosition eye{up * (radius + grown.ground + 1.5)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, grown.ground);
    const double elevation = 40.0 * std::numbers::pi / 180.0;
    const DVec3 sun = (east * std::cos(elevation)) + (up * std::sin(elevation));
    const DVec3 ahead = grown.looking_away ? -east : east;
    const hog::render::Camera camera{
        .eye = eye, .forward = glm::normalize(ahead - (up * 0.35)), .up = up, .vertical_fov = 1.2};
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), bend);
    const hog::render::SkyView seen{.camera = camera,
                                    .sun = sun,
                                    .bend = bend,
                                    .exposure = 10.0,
                                    .sky_light = model.sky_irradiance(2.0 * scale, sun),
                                    .gusts = {.anchor = DVec3{1'000.0} - grown.gusts_drifted,
                                              .along = east,
                                              .wave = -grown.seconds}};
    const GrassDraw lawn{.slot = slot, .origin = origin, .spacing = spacing, .cell = field_cell};
    const GrassView view{.camera = camera,
                         .bend = bend,
                         .air = sky->bindings(0, *atmosphere),
                         .wind = grown.wind,
                         .seconds = grown.seconds};

    auto colour = hog::render::make_image(
        device, {.format = colour_format,
                 .extent = frame,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(colour);
    auto depth =
        hog::render::make_image(device, {.format = depth_format,
                                         .extent = frame,
                                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
    REQUIRE(depth);
    auto count = hog::render::make_buffer(device, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          hog::render::Memory::Readback);
    REQUIRE(count);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        CHECK(pool->record_uploads(commands, 0));
        CHECK(sky->record_frame(commands, frame, 0, *atmosphere, seen));
        grass->grow(commands, frame, *pool, {&lawn, 1}, view);
        hog::render::transition(commands, colour->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, colour->view(), frame,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view(), .clear = 0.0F});
        grass->draw(commands, frame, view);
        vkCmdEndRendering(commands);
        hog::render::memory_barrier(commands, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                                    VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                                    VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy whole{.srcOffset = 0, .dstOffset = 0, .size = 16};
        vkCmdCopyBuffer(commands, grass->grown_draw(), count->handle(), 1, &whole);
        hog::render::memory_barrier(commands, VK_PIPELINE_STAGE_2_COPY_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                    VK_ACCESS_2_HOST_READ_BIT);
    }));
    if (counted != nullptr) {
        REQUIRE(count->invalidate());
        std::array<std::uint32_t, 4> arguments{};
        std::memcpy(arguments.data(), count->mapped().data(), sizeof(arguments));
        CHECK(arguments[0] == 6);
        *counted = arguments[1];
    }
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

} // namespace

TEST_CASE("grass grows over grassland and not over the sea", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const Pixels grassland = draw(*device, {.biome = hog::planet::Biome::Grassland});
    const std::size_t lit = lit_pixels(grassland);
    CHECK(lit > 100);
    // Green blades: grass, not glare.
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    for (std::size_t pixel = 0; pixel < grassland.size() / 4; ++pixel) {
        red += grassland.at(4 * pixel);
        green += grassland.at((4 * pixel) + 1);
        blue += grassland.at((4 * pixel) + 2);
    }
    INFO("red " << red << ", green " << green << ", blue " << blue);
    CHECK(green > 1.2 * red);
    CHECK(red > blue);
    CHECK(lit_pixels(draw(*device, {.biome = hog::planet::Biome::Ocean})) == 0);
    // Desert bunchgrass stands in lone tufts: far fewer blades than a grassland, but some.
    const std::size_t tufts = lit_pixels(draw(*device, {.biome = hog::planet::Biome::Desert}));
    CHECK(tufts > 0);
    CHECK(tufts < lit / 4);
}

TEST_CASE("thinner grass is the thicker grass with blades left out", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // Held at full density all the way, no blade drawn wider to stand for others.
    const Pixels thin = draw(*device, {.grass = {.density = 12.0, .hold = 1'000.0}});
    const Pixels thick = draw(*device, {.grass = {.density = 36.0, .hold = 1'000.0}});
    CHECK(lit_pixels(thick) > lit_pixels(thin));
    // Every blade of the thin field stands in the thick one too, where it was.
    std::size_t lost = 0;
    for (std::size_t pixel = 0; pixel < thin.size() / 4; ++pixel) {
        const bool was = thin.at(4 * pixel) + thin.at((4 * pixel) + 1) > 0;
        const bool is = thick.at(4 * pixel) + thick.at((4 * pixel) + 1) > 0;
        lost += was && !is ? 1U : 0U;
    }
    CHECK(lost == 0);
}

TEST_CASE("a cell grows the full density out to its hold, thinning with distance squared",
          "[render]") {
    const hog::render::GrassTier tier{.density = 40.0, .hold = 20.0, .reach = 100.0};
    CHECK(hog::render::grass_layers(tier, 0.5, 0.0) == 10.0);
    CHECK(hog::render::grass_layers(tier, 0.5, 20.0) == 10.0);
    CHECK(hog::render::grass_layers(tier, 0.5, 40.0) == 2.5);
    CHECK(hog::render::grass_layers(tier, 1.0, 80.0) == 2.5);
    // Thicker grass, thicker only until it's thinned to the rest of the field's density.
    CHECK(hog::render::grass_layers(tier, 0.5, 0.0, 4.0) == 40.0);
    CHECK(hog::render::grass_layers(tier, 0.5, 20.0, 4.0) == 2.5 * 4.0);
    CHECK(hog::render::grass_layers(tier, 0.5, 40.0, 4.0) == 2.5);
    CHECK(hog::render::grass_layers(tier, 1.0, 80.0, 4.0) == 2.5);
}

TEST_CASE("thinned grass is drawn wider, covering the ground near as thickly", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // Finer, so the widened blades span pixels of their own.
    constexpr VkExtent2D fine{.width = 384, .height = 216};
    const auto lit = [&](const hog::render::GrassTier& grass) {
        return lit_pixels(draw(*device, {.away = 10.0, .grass = grass, .size = fine}));
    };
    const std::size_t full = lit({.density = 40.0, .hold = 1'000.0});
    // Thinning from 6 m, to a quarter by 12 m.
    const std::size_t thinned = lit({.density = 40.0, .hold = 6.0});
    // As few blades, each drawn as it is.
    const std::size_t sparse = lit({.density = 10.0, .hold = 1'000.0});
    INFO("full " << full << ", thinned " << thinned << ", sparse " << sparse);
    CHECK(thinned > sparse);
    CHECK(thinned > full / 2);
    CHECK(thinned < full * 3 / 2);
}

TEST_CASE("grass strays past its own biome, thinning with that biome's share", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::planet::Biome;
    const std::size_t grassland = lit_pixels(draw(*device, {.biome = Biome::Grassland}));
    const std::size_t half = lit_pixels(
        draw(*device, {.biome = Biome::Ocean, .blend_biome = Biome::Grassland, .blend = 128}));
    const std::size_t fringe = lit_pixels(
        draw(*device, {.biome = Biome::Ocean, .blend_biome = Biome::Grassland, .blend = 32}));
    INFO("grassland " << grassland << ", half " << half << ", fringe " << fringe);
    CHECK(half > grassland / 4);
    CHECK(half < grassland);
    CHECK(fringe > 0);
    CHECK(fringe < half);
}

TEST_CASE("grass takes the ground where the trees are gone, once a fire's black has greened",
          "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::planet::Biome;
    const std::size_t shaded = lit_pixels(draw(*device, {.biome = Biome::TemperateForest}));
    const std::size_t open =
        lit_pixels(draw(*device, {.biome = Biome::TemperateForest, .cover = {0, 0xf0}}));
    INFO("under the trees " << shaded << ", the trees gone " << open);
    CHECK(shaded > 0);
    CHECK(open > 2 * shaded);
    CHECK(lit_pixels(draw(*device, {.biome = Biome::TemperateForest, .cover = {255, 0xf0}})) == 0);
    const std::size_t greening =
        lit_pixels(draw(*device, {.biome = Biome::TemperateForest, .cover = {128, 0xf0}}));
    CHECK(greening > 0);
    CHECK(greening < open);
}

TEST_CASE("reeds fringe the waterline and leave higher ground alone", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // A lake's shore, inside the reed band: stems with brown cattail heads.
    const Pixels fringe = draw(*device, {.biome = hog::planet::Biome::Lake, .ground = 0.4});
    CHECK(lit_pixels(fringe) > 50);
    std::size_t brown = 0;
    for (std::size_t pixel = 0; pixel < fringe.size() / 4; ++pixel) {
        const std::uint8_t red = fringe.at(4 * pixel);
        const std::uint8_t green = fringe.at((4 * pixel) + 1);
        brown += (red > 0 && red > green) ? 1U : 0U;
    }
    CHECK(brown > 10);
    // Above the band, a lake's ground grows nothing at all.
    CHECK(lit_pixels(draw(*device, {.biome = hog::planet::Biome::Lake, .ground = 2.0})) == 0);
}

TEST_CASE("tall grass reaches further than short, and none beyond the reach", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // Fine enough that tall blades this far off span pixels.
    constexpr VkExtent2D fine{.width = 384, .height = 216};
    using enum hog::planet::Biome;
    CHECK(lit_pixels(draw(*device, {.biome = Tundra, .size = fine})) > 0);
    // 70 m out short tundra grass is under a pixel tall and gone, and tall grassland stands.
    CHECK(lit_pixels(draw(*device, {.biome = Tundra, .away = 70.0, .size = fine})) == 0);
    CHECK(lit_pixels(draw(*device, {.biome = Grassland, .away = 70.0, .size = fine})) > 0);
    CHECK(lit_pixels(draw(
              *device,
              {.biome = Grassland, .away = 70.0, .grass = {.reach = 50.0}, .size = fine})) == 0);
}

TEST_CASE("the wind moves the grass, and still air leaves it still", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const DVec3 wind = east * 6.0;
    const Pixels now = draw(*device, {.wind = wind, .seconds = 1.0});
    const Pixels later = draw(*device, {.wind = wind, .seconds = 1.4});
    CHECK(now != later);
    const Pixels still_now = draw(*device, {.seconds = 1.0});
    const Pixels still_later = draw(*device, {.seconds = 1.4});
    CHECK(still_now == still_later);
}

TEST_CASE("gusts move the grass as they drift over it", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const DVec3 wind = east * 6.0;
    const DVec3 drifted = east * 30.0;
    CHECK(draw(*device, {.wind = wind, .seconds = 1.0}) !=
          draw(*device, {.wind = wind, .seconds = 1.0, .gusts_drifted = drifted}));
    CHECK(draw(*device, {.seconds = 1.0}) ==
          draw(*device, {.seconds = 1.0, .gusts_drifted = drifted}));
}

TEST_CASE("meadows of tall grass wave in the wind, and lawns, bare ground and water don't",
          "[render]") {
    using enum hog::planet::Biome;
    using hog::render::waving_grass;
    CHECK(waving_grass(Grassland) == 1.0F);
    CHECK(waving_grass(Savanna) > 0.7F);
    CHECK(waving_grass(Wetland) > 0.7F);
    CHECK(waving_grass(Shrubland) < 0.3F);
    CHECK(waving_grass(Desert) < 0.1F);
    for (const hog::planet::Biome bare : {Tundra, Alpine, Ocean, Beach, Ice, Lake}) {
        CHECK(waving_grass(bare) == 0.0F);
    }
}

TEST_CASE("only grass that may be seen is grown", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    std::uint32_t ahead = 0;
    const Pixels seen = draw(*device, {.away = 10.0}, &ahead);
    CHECK(lit_pixels(seen) > 0);
    CHECK(ahead > 0);
    CHECK(ahead < hog::render::most_grown_blades);
    std::uint32_t behind = 0;
    const Pixels unseen = draw(*device, {.away = 10.0, .looking_away = true}, &behind);
    CHECK(lit_pixels(unseen) == 0);
    CHECK(behind == 0);
}

TEST_CASE("the field frays away towards the end of its reach rather than stopping along a line",
          "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // Wide enough that blades stand many pixels tall right out to the end of the reach.
    constexpr VkExtent2D wide{.width = 960, .height = 540};
    const auto grown = [&](double away, double reach) {
        std::uint32_t count = 0;
        draw(*device, {.away = away, .grass = {.reach = reach}, .size = wide}, &count);
        return count;
    };
    // Well short of where any blade ends, all grow.
    CHECK(grown(8.0, 40.0) == grown(8.0, 400.0));
    // Towards the end of the reach, some have ended and some not.
    const double left = static_cast<double>(grown(28.0, 40.0)) / grown(28.0, 400.0);
    INFO(left << " of the blades left");
    CHECK(left > 0.2);
    CHECK(left < 0.85);
}

TEST_CASE("a patch's blades are sown as deep as its grid cells grow them", "[render]") {
    using hog::render::grass_depth;
    // Its grid cells, five levels in, grow one each; each level further four times as many.
    CHECK(grass_depth(1.0, 10) == 5);
    CHECK(grass_depth(1.1, 10) == 6);
    CHECK(grass_depth(4.0, 10) == 6);
    CHECK(grass_depth(5.0, 10) == 7);
    CHECK(grass_depth(64.0, 10) == 8);
    CHECK(grass_depth(0.25, 10) == 4);
    // A blade or none for the whole patch, far off.
    CHECK(grass_depth(1.0 / 1024.0, 10) == 0);
    CHECK(grass_depth(1e-9, 10) == 0);
    // No deeper than most_grass_depth, nor than the lattice.
    CHECK(grass_depth(1'000.0, 10) == hog::render::most_grass_depth);
    CHECK(grass_depth(1'000.0, 6) == 6);
}

TEST_CASE("a patch's record bounds how thick its grass grows", "[grass]") {
    for (const double layers : {1e-6, 0.01, 0.3, 1.0, 1.1, 5.0, 64.0, 900.0}) {
        const std::uint32_t record = hog::render::grass_patch_level(13, layers);
        CHECK((record & 0xffU) == 13U);
        const double half_log = (static_cast<double>(record >> 8U) - 128.0) / 8.0;
        INFO(layers << " layers, " << half_log);
        CHECK(half_log >= std::log2(layers) / 2.0);
        CHECK(half_log < (std::log2(layers) / 2.0) + 0.125);
    }
    // Clamped either way, never under.
    CHECK((hog::render::grass_patch_level(0, 1e-300) >> 8U) == 0U);
    CHECK((hog::render::grass_patch_level(0, 1e300) >> 8U) == 255U);
}

namespace {

// One patch of a flat grassland: its cell, its centre east and north of the field's, and the
// metres between its grid vertices.
struct Piece {
    FaceIJ cell;
    double east = 0.0;
    double north = 0.0;
    double across = spacing;
};

// Where the blades grown on `pieces` of a flat grassland stand, from the eye, seen as draw()
// sees a field 10 m off.
std::vector<Vec3> grown_roots(hog::render::Device& device, std::span<const Piece> pieces,
                              const hog::render::GrassTier& tier) {
    constexpr VkExtent2D frame{.width = 384, .height = 216};
    constexpr std::uint32_t most = 1U << 17U;
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    std::array<Vec3, hog::planet::biome_count> palette{};
    palette.fill(Vec3{0.35F, 0.55F, 0.25F});
    auto grass = Grass::create(device, colour_format, depth_format, palette, lattice, tier);
    REQUIRE(grass);
    const auto count = static_cast<std::uint32_t>(pieces.size());
    auto pool =
        hog::render::TerrainPool::create(device, {.patches = count, .frames = 1, .uploads = count});
    REQUIRE(pool);

    const DVec3 north{0.0, 1.0, 0.0};
    const WorldPosition eye{up * (radius + ground_height + 1.5)};
    std::vector<GrassDraw> draws;
    for (const Piece& piece : pieces) {
        const std::uint32_t slot = pool->acquire().value_or(99);
        REQUIRE(pool->stage(
            0, slot,
            field(hog::planet::Biome::Grassland, {}, hog::planet::Biome::Ocean, 0, piece.across)));
        const DVec3 centre = (east * (10.0 + piece.east)) + (north * piece.north);
        const double reach = piece.across * hog::terrain::patch_cells * std::numbers::sqrt2 / 2.0;
        draws.push_back({.slot = slot,
                         .origin = WorldPosition{(up * (radius + ground_height)) + centre},
                         .spacing = piece.across,
                         .cell = piece.cell,
                         .nearest = std::max(0.0, glm::length(centre) - reach)});
    }
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, ground_height);
    const hog::render::Camera camera{
        .eye = eye, .forward = glm::normalize(east - (up * 0.35)), .up = up, .vertical_fov = 1.2};
    const hog::atmosphere::Model model;
    const DVec3 sun = glm::normalize(east + up);
    const hog::render::SkyView seen{
        .camera = camera,
        .sun = sun,
        .bend = bend,
        .exposure = 10.0,
        .sky_light = model.sky_irradiance(2.0 * hog::render::air_scale(model.air(), bend), sun),
        .gusts = {}};
    const GrassView view{.camera = camera, .bend = bend, .air = sky->bindings(0, *atmosphere)};

    constexpr VkDeviceSize arguments_bytes = 16;
    auto grown = hog::render::make_buffer(
        device, arguments_bytes + (VkDeviceSize{most} * hog::render::grown_blade_bytes),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, hog::render::Memory::Readback);
    REQUIRE(grown);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        CHECK(pool->record_uploads(commands, 0));
        CHECK(sky->record_frame(commands, frame, 0, *atmosphere, seen));
        grass->grow(commands, frame, *pool, draws, view);
        hog::render::memory_barrier(commands, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                                    VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                                    VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy arguments{.srcOffset = 0, .dstOffset = 0, .size = arguments_bytes};
        vkCmdCopyBuffer(commands, grass->grown_draw(), grown->handle(), 1, &arguments);
        const VkBufferCopy blades{.srcOffset = 0,
                                  .dstOffset = arguments_bytes,
                                  .size = VkDeviceSize{most} * hog::render::grown_blade_bytes};
        vkCmdCopyBuffer(commands, grass->grown_blades(), grown->handle(), 1, &blades);
        hog::render::memory_barrier(commands, VK_PIPELINE_STAGE_2_COPY_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                    VK_ACCESS_2_HOST_READ_BIT);
    }));
    CHECK(device.validation_errors() == 0);
    REQUIRE(grown->invalidate());
    const std::span<const std::byte> read = grown->mapped();
    std::uint32_t blades = 0;
    std::memcpy(&blades, read.subspan(sizeof(std::uint32_t)).data(), sizeof(blades));
    REQUIRE(blades <= most);
    std::vector<Vec3> roots(blades);
    for (std::size_t at = 0; at < roots.size(); ++at) {
        std::memcpy(&roots[at],
                    read.subspan(arguments_bytes + (at * hog::render::grown_blade_bytes)).data(),
                    sizeof(Vec3));
    }
    std::ranges::sort(roots, {}, [](const Vec3& root) { return root.x; });
    return roots;
}

// How many of `roots` have none of `others` standing within a millimetre of them.
std::size_t unmatched(const std::vector<Vec3>& roots, const std::vector<Vec3>& others) {
    constexpr float close = 1e-3F;
    std::size_t lone = 0;
    for (const Vec3& root : roots) {
        const auto from = std::ranges::lower_bound(others, root.x - close, {},
                                                   [](const Vec3& other) { return other.x; });
        bool matched = false;
        for (auto at = from; at != others.end() && at->x <= root.x + close && !matched; ++at) {
            matched = glm::length(*at - root) < close;
        }
        lone += matched ? 0U : 1U;
    }
    return lone;
}

} // namespace

TEST_CASE("a patch grows the very blades its four children do", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const Piece whole{.cell = field_cell};
    std::vector<Piece> quarters;
    for (std::uint32_t child = 0; child < 4; ++child) {
        const std::uint32_t across = child & 1U;
        const std::uint32_t up_it = child >> 1U;
        const double offset = hog::terrain::patch_cells * spacing / 4.0;
        quarters.push_back({.cell = {.face = field_cell.face,
                                     .i = (field_cell.i << 1U) | across,
                                     .j = (field_cell.j << 1U) | up_it,
                                     .level = field_cell.level + 1},
                            .east = across == 0 ? -offset : offset,
                            .north = up_it == 0 ? -offset : offset,
                            .across = spacing / 2.0});
    }
    // Held thick throughout, and thinning from 3 m, each child sown as deep as its own nearest
    // blades need.
    for (const double hold : {1'000.0, 3.0}) {
        const hog::render::GrassTier tier{.density = 40.0, .hold = hold};
        const std::vector<Vec3> parent = grown_roots(*device, {&whole, 1}, tier);
        const std::vector<Vec3> children = grown_roots(*device, quarters, tier);
        INFO("held to " << hold << " m: " << parent.size() << " blades grown on the patch, "
                        << children.size() << " on its children");
        CHECK(parent.size() > 1'000);
        CHECK(parent.size() == children.size());
        CHECK(unmatched(parent, children) == 0);
        CHECK(unmatched(children, parent) == 0);
    }
}
