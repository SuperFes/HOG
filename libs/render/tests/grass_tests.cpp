#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/planet/climate.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/grass.hpp>
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
#include <numbers>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::Vec3;
using hog::WorldPosition;
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

// A flat patch of one biome, or two blended, level at `ground_height`, centred on its origin.
std::vector<hog::terrain::PatchVertex>
field(hog::planet::Biome biome, hog::terrain::Cover cover = {},
      hog::planet::Biome blend_biome = hog::planet::Biome::Ocean, std::uint8_t blend = 0) {
    std::vector<hog::terrain::PatchVertex> vertices(hog::render::patch_vertex_count);
    constexpr int middle = hog::terrain::patch_cells / 2;
    for (int i = 0; i < hog::terrain::patch_side; ++i) {
        for (int j = 0; j < hog::terrain::patch_side; ++j) {
            const std::size_t at = (static_cast<std::size_t>(i) * hog::terrain::patch_side) +
                                   static_cast<std::size_t>(j);
            vertices[at] = {.position = Vec3{static_cast<float>((i - middle) * spacing),
                                             static_cast<float>((j - middle) * spacing), 0.0F},
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
    hog::render::GrassTier lawn{};
    hog::render::GrassTier swaths = hog::render::swath_tier;
    bool tall_only = false;
    double ground = ground_height; // metres above sea level
    hog::terrain::Cover cover{};
    hog::planet::Biome blend_biome = hog::planet::Biome::Ocean;
    std::uint8_t blend = 0;
    VkExtent2D size = extent;
};

// The field's grass alone over black, seen from standing height looking slightly down, east.
Pixels draw(hog::render::Device& device, const Field& grown) {
    const VkExtent2D frame = grown.size;
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    std::array<Vec3, hog::planet::biome_count> palette{};
    palette.fill(Vec3{0.35F, 0.55F, 0.25F});
    auto grass =
        Grass::create(device, colour_format, depth_format, palette, grown.lawn, grown.swaths);
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
    const hog::render::Camera camera{
        .eye = eye, .forward = glm::normalize(east - (up * 0.35)), .up = up, .vertical_fov = 1.2};
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), bend);
    const hog::render::SkyView seen{.camera = camera,
                                    .sun = sun,
                                    .bend = bend,
                                    .exposure = 10.0,
                                    .sky_light = model.sky_irradiance(2.0 * scale, sun),
                                    .gusts_drifted = grown.gusts_drifted};
    const GrassDraw lawn{.slot = slot,
                         .origin = origin,
                         .spacing = spacing,
                         .seed = 7,
                         .tall_only = grown.tall_only};
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
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        CHECK(pool->record_uploads(commands, 0));
        CHECK(sky->record_frame(commands, frame, 0, *atmosphere, seen));
        hog::render::transition(commands, colour->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, colour->view(), frame,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view(), .clear = 0.0F});
        grass->draw(commands, frame, *pool, {&lawn, 1}, view);
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
    const Pixels thin = draw(*device, {.lawn = {.density = 12.0, .hold = 1'000.0}});
    const Pixels thick = draw(*device, {.lawn = {.density = 36.0, .hold = 1'000.0}});
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

TEST_CASE("a cell grows its tier's density out to its hold, thinning with distance squared",
          "[render]") {
    const hog::render::GrassTier tier{.density = 40.0, .hold = 20.0, .far = 100.0};
    CHECK(hog::render::grass_layers(tier, 0.5, 0.0) == 10.0);
    CHECK(hog::render::grass_layers(tier, 0.5, 20.0) == 10.0);
    CHECK(hog::render::grass_layers(tier, 0.5, 40.0) == 2.5);
    CHECK(hog::render::grass_layers(tier, 1.0, 80.0) == 2.5);
}

TEST_CASE("thinned grass is drawn wider, covering the ground near as thickly", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // Finer, so the widened blades span pixels of their own.
    constexpr VkExtent2D fine{.width = 384, .height = 216};
    const auto lit = [&](const hog::render::GrassTier& lawn) {
        return lit_pixels(draw(*device, {.away = 10.0, .lawn = lawn, .size = fine}));
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

TEST_CASE("tall swaths stand where the lawn has faded away", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // Fine enough that blades this far off span pixels.
    constexpr VkExtent2D fine{.width = 384, .height = 216};
    // 70 m out the lawn's fade has taken it; a far-swath draw holds its full height.
    const Field faded{.biome = hog::planet::Biome::Grassland, .away = 70.0, .size = fine};
    CHECK(lit_pixels(draw(*device, faded)) == 0);
    const Field swath{
        .biome = hog::planet::Biome::Grassland, .away = 70.0, .tall_only = true, .size = fine};
    CHECK(lit_pixels(draw(*device, swath)) > 0);
    // Short grass stays a near effect: no tundra in the far swaths.
    const Field tundra{
        .biome = hog::planet::Biome::Tundra, .away = 70.0, .tall_only = true, .size = fine};
    CHECK(lit_pixels(draw(*device, tundra)) == 0);
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
