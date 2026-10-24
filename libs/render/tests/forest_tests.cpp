#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/flora/species.hpp>
#include <hog/render/anchors.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/forest.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/trees.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::Vec3;
using hog::WorldPosition;
using hog::render::AnchorPool;
using hog::render::Forest;
using hog::render::GrassView;
using hog::render::StandDraw;
using hog::render::TreeAnchor;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 96, .height = 54};
constexpr VkFormat colour_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr double radius = 115'000.0;
constexpr double ground_height = 2.0;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};
constexpr DVec3 north{0.0, 1.0, 0.0};
constexpr Vec3 needles{0.08F, 0.15F, 0.08F};

using Pixels = std::vector<std::uint8_t>;

std::uint32_t hashed(std::uint32_t at) {
    at ^= at >> 16U;
    at *= 0x7feb352dU;
    at ^= at >> 15U;
    at *= 0x846ca68bU;
    at ^= at >> 16U;
    return at;
}

// A rank of trees anchored east to west, `away` metres north of the tile's origin.
std::vector<TreeAnchor> rank_of(int count, double away, float height = 11.0F,
                                float age = hog::flora::prime_age,
                                hog::flora::Genus genus = hog::flora::Genus::Pine) {
    std::vector<TreeAnchor> anchors;
    for (int tree = 0; tree < count; ++tree) {
        const double spread = (static_cast<double>(tree) - ((count - 1) * 0.5)) * 8.0;
        anchors.push_back({.position = Vec3{spread, away, 0.0},
                           .height = height,
                           .normal = Vec3{0.0F, 0.0F, 1.0F},
                           .hash = hashed(static_cast<std::uint32_t>(tree) * 2654435761U),
                           .colour = hog::render::packed_foliage(needles, genus),
                           .age = age});
    }
    return anchors;
}

struct Stand {
    std::vector<TreeAnchor> anchors;
    int detail = 0;
    DVec3 wind{0.0};
    double seconds = 0.0;
    bool with_stamps = false; // the stamp tier too, held off to the forest's reach
    double eye_up = 6.0;
    double eye_back = -40.0;
    hog::flora::Daylight daylight{};
    DVec3 gusts_drifted{0.0};
};

// The stand's grown trees over black, from south of the origin facing north.
Pixels draw(hog::render::Device& device, const Stand& stand) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    auto forest = Forest::create(device, colour_format, depth_format, 80.0, 150.0);
    REQUIRE(forest);
    auto stamps =
        hog::render::Trees::create(device, colour_format, depth_format, hog::render::forest_far);
    REQUIRE(stamps);
    auto pool = AnchorPool::create(device, {.tiles = 1, .frames = 1, .uploads = 1});
    REQUIRE(pool);
    const std::uint32_t slot = pool->acquire().value_or(99);
    REQUIRE(slot == 0);
    REQUIRE(pool->stage(0, slot, stand.anchors));

    const WorldPosition origin{up * (radius + ground_height)};
    const WorldPosition eye{(up * (radius + ground_height + stand.eye_up)) +
                            (north * stand.eye_back)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, ground_height);
    const double elevation = 40.0 * std::numbers::pi / 180.0;
    const DVec3 sun = (east * std::cos(elevation)) + (up * std::sin(elevation));
    const hog::render::Camera camera{.eye = eye, .forward = north, .up = up, .vertical_fov = 0.6};
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), bend);
    const hog::render::SkyView seen{
        .camera = camera,
        .sun = sun,
        .bend = bend,
        .exposure = 10.0,
        .sky_light = model.sky_irradiance(2.0 * scale, sun),
        .daylight = stand.daylight,
        .gusts = {.anchor = DVec3{1'000.0} - stand.gusts_drifted, .along = east}};
    const StandDraw grown{.slot = slot,
                          .count = static_cast<std::uint32_t>(stand.anchors.size()),
                          .origin = origin,
                          .detail = stand.detail,
                          .genus = stand.anchors.empty()
                                       ? hog::flora::Genus::Pine
                                       : hog::render::genus_of(stand.anchors.front().colour)};
    const GrassView view{.camera = camera,
                         .bend = bend,
                         .air = sky->bindings(0, *atmosphere),
                         .wind = stand.wind,
                         .seconds = stand.seconds};

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
        forest->draw(commands, extent, *pool, {&grown, 1}, view);
        if (stand.with_stamps) {
            stamps->draw(commands, extent, *pool, {&grown, 1}, view);
        }
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

// Columns of the picture any tree covers.
std::size_t columns_lit(const Pixels& pixels) {
    std::size_t lit = 0;
    for (std::uint32_t column = 0; column < extent.width; ++column) {
        for (std::uint32_t row = 0; row < extent.height; ++row) {
            const std::size_t at = 4 * ((std::size_t{row} * extent.width) + column);
            if (pixels.at(at) + pixels.at(at + 1) + pixels.at(at + 2) > 0) {
                ++lit;
                break;
            }
        }
    }
    return lit;
}

// Pixels `part` lights that `whole` leaves dark.
std::size_t stray_pixels(const Pixels& part, const Pixels& whole) {
    std::size_t stray = 0;
    for (std::size_t at = 0; at + 3 < part.size(); at += 4) {
        const bool lit = part.at(at) + part.at(at + 1) + part.at(at + 2) > 0;
        const bool covered = whole.at(at) + whole.at(at + 1) + whole.at(at + 2) > 0;
        stray += (lit && !covered) ? 1U : 0U;
    }
    return stray;
}

std::size_t barky_pixels(const Pixels& pixels) {
    std::size_t barky = 0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
        const int red = pixels.at(4 * pixel);
        const int green = pixels.at((4 * pixel) + 1);
        const int blue = pixels.at((4 * pixel) + 2);
        barky += (red > 20 && red >= green && red > blue) ? 1U : 0U;
    }
    return barky;
}

// Counted from row `from` down, the picture's top row first.
std::size_t leafy_pixels(const Pixels& pixels, std::uint32_t from = 0) {
    std::size_t leafy = 0;
    for (std::size_t pixel = std::size_t{from} * extent.width; pixel < pixels.size() / 4; ++pixel) {
        const int red = pixels.at(4 * pixel);
        const int green = pixels.at((4 * pixel) + 1);
        leafy += (green > 20 && green > red) ? 1U : 0U;
    }
    return leafy;
}

// The lowest row of the picture any leaves cover, counted from the top; 0 for none.
std::uint32_t lowest_leafy_row(const Pixels& pixels) {
    std::uint32_t lowest = 0;
    for (std::uint32_t row = 0; row < extent.height; ++row) {
        for (std::uint32_t column = 0; column < extent.width; ++column) {
            const std::size_t at = 4 * ((std::size_t{row} * extent.width) + column);
            if (pixels.at(at + 1) > 20 && pixels.at(at + 1) > pixels.at(at)) {
                lowest = row;
                break;
            }
        }
    }
    return lowest;
}

// Bark below row `from`, counted from the top.
std::size_t barky_below(const Pixels& pixels, std::uint32_t from) {
    const auto begin = static_cast<std::ptrdiff_t>(4 * std::size_t{from + 1} * extent.width);
    return barky_pixels(Pixels(pixels.begin() + begin, pixels.end()));
}

// Leaves gone yellow, orange or red: warm and well saturated, as no bark is.
std::size_t autumnal_pixels(const Pixels& pixels) {
    std::size_t autumnal = 0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
        const int red = pixels.at(4 * pixel);
        const int green = pixels.at((4 * pixel) + 1);
        const int blue = pixels.at((4 * pixel) + 2);
        autumnal += (red > 20 && red > green && red > 2 * blue) ? 1U : 0U;
    }
    return autumnal;
}

// The same anchors in another condition.
std::vector<TreeAnchor> fared(std::vector<TreeAnchor> anchors, hog::flora::Condition condition) {
    for (TreeAnchor& anchor : anchors) {
        anchor.colour =
            hog::render::packed_foliage(hog::render::foliage_of(anchor.colour),
                                        hog::render::genus_of(anchor.colour), condition);
    }
    return anchors;
}

// The mean of red, green and blue over the pixels lit, 0 to 255.
double mean_brightness(const Pixels& pixels) {
    double sum = 0.0;
    double lit = 0.0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
        const int brightness =
            pixels.at(4 * pixel) + pixels.at((4 * pixel) + 1) + pixels.at((4 * pixel) + 2);
        if (brightness > 0) {
            sum += brightness / 3.0;
            lit += 1.0;
        }
    }
    return lit > 0.0 ? sum / lit : 0.0;
}

} // namespace

TEST_CASE("grown pines carry trunks and needles on their anchors", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const Pixels stand = draw(*device, {.anchors = rank_of(5, 0.0), .detail = 0});
    CHECK(lit_pixels(stand) > 300);
    // Only the trunk is wood now: each twig's frond runs its own spine.
    CHECK(barky_pixels(stand) > 4);
    CHECK(leafy_pixels(stand) > 100);
    // An empty tile grows nothing.
    CHECK(lit_pixels(draw(*device, {.anchors = {}})) == 0);
}

TEST_CASE("gusts move the grown trees' crowns as they drift over them", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const DVec3 wind{14.0, 0.0, 0.0};
    const DVec3 drifted{30.0, 0.0, 0.0};
    CHECK(
        draw(*device, {.anchors = rank_of(5, 0.0), .wind = wind, .seconds = 1.0}) !=
        draw(*device,
             {.anchors = rank_of(5, 0.0), .wind = wind, .seconds = 1.0, .gusts_drifted = drifted}));
    CHECK(draw(*device, {.anchors = rank_of(5, 0.0), .seconds = 1.0}) ==
          draw(*device, {.anchors = rank_of(5, 0.0), .seconds = 1.0, .gusts_drifted = drifted}));
}

TEST_CASE("the baked details thin with distance, holding the stand", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const Pixels full = draw(*device, {.anchors = rank_of(5, 0.0), .detail = 0});
    const Pixels coarse = draw(*device, {.anchors = rank_of(5, 0.0), .detail = 2});
    CHECK(lit_pixels(full) > 300);
    // Coarser, yet still holding most of the stand's cover.
    CHECK(lit_pixels(coarse) > lit_pixels(full) / 2);
    CHECK(full != coarse);
}

TEST_CASE("the forest ends at its reach, the stamps carrying on from the same anchors",
          "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // One rank within the forest's reach, one beyond it, seen from high enough to hold both: the
    // grown tier draws only the near rank, the stamps only the far one, and together they cover
    // both — one set of anchored trees, two representations.
    std::vector<TreeAnchor> ranks = rank_of(3, 100.0);
    const std::vector<TreeAnchor> far_rank = rank_of(5, 260.0, 16.0F);
    ranks.insert(ranks.end(), far_rank.begin(), far_rank.end());
    const Stand eyes{.anchors = ranks, .detail = 2, .eye_up = 30.0, .eye_back = -60.0};
    const std::size_t grown_alone = lit_pixels(draw(*device, eyes));
    Stand both = eyes;
    both.with_stamps = true;
    const std::size_t together = lit_pixels(draw(*device, both));
    CHECK(grown_alone > 5);
    CHECK(together > grown_alone + 5);
}

TEST_CASE("each tree grows as its age: saplings green to the ground, old trees bare below",
          "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // The same height at either age, so only the shape differs.
    const Pixels saplings = draw(*device, {.anchors = rank_of(5, 0.0, 11.0F, 0.1F)});
    const Pixels old = draw(*device, {.anchors = rank_of(5, 0.0, 11.0F, 0.97F)});
    CHECK(leafy_pixels(saplings) > 100);
    CHECK(leafy_pixels(old) > 100);
    // The lowest few metres of the trees, above the ground the eye looks down to.
    constexpr std::uint32_t low = 34;
    CHECK(leafy_pixels(saplings, low) > 30);
    CHECK(leafy_pixels(saplings, low) > 2 * leafy_pixels(old, low));
}

TEST_CASE("an oak grows a broad crown of leaves where a pine of its height stands narrow",
          "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::flora::Genus;
    const Pixels pine = draw(*device, {.anchors = rank_of(1, 0.0, 11.0F, hog::flora::prime_age)});
    const Pixels oak =
        draw(*device, {.anchors = rank_of(1, 0.0, 11.0F, hog::flora::prime_age, Genus::Oak)});
    CHECK(leafy_pixels(oak) > 60);
    CHECK(barky_pixels(oak) > 2);
    CHECK(columns_lit(oak) > columns_lit(pine) + (columns_lit(pine) / 2));
}

TEST_CASE("an oak's leaves turn in autumn and are gone by winter, a pine's needles kept",
          "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::flora::Genus;
    // Earth's days at 45 degrees north: a July day, one in late October, and January's.
    constexpr hog::flora::Daylight summer{.hours = 15.0F, .longest = 15.4F};
    constexpr hog::flora::Daylight autumn{.hours = 10.7F, .longest = 15.4F};
    constexpr hog::flora::Daylight winter{.hours = 9.0F, .longest = 15.4F, .lengthening = true};
    const auto oaks = rank_of(3, 0.0, 11.0F, hog::flora::prime_age, Genus::Oak);
    const Pixels green = draw(*device, {.anchors = oaks, .daylight = summer});
    const Pixels turning = draw(*device, {.anchors = oaks, .daylight = autumn});
    const Pixels bare = draw(*device, {.anchors = oaks, .daylight = winter});
    CHECK(leafy_pixels(green) > 150);
    CHECK(autumnal_pixels(green) < leafy_pixels(green) / 20);
    CHECK(autumnal_pixels(turning) > 100);
    CHECK(autumnal_pixels(turning) > leafy_pixels(turning));
    CHECK(leafy_pixels(bare) < leafy_pixels(green) / 20);
    CHECK(autumnal_pixels(bare) < autumnal_pixels(turning) / 10);
    CHECK(barky_pixels(bare) > 10);

    const auto pines = rank_of(3, 0.0);
    CHECK(leafy_pixels(draw(*device, {.anchors = pines, .daylight = winter})) ==
          leafy_pixels(draw(*device, {.anchors = pines, .daylight = summer})));
}

TEST_CASE("a crown losing its leaves drops whole fronds, none stretched across the sky",
          "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::flora::Genus;
    // Off to the side, clear of the picture's middle, where a torn frond's dropped corners would
    // pull it.
    const std::vector<TreeAnchor> oak{
        rank_of(3, 0.0, 11.0F, hog::flora::prime_age, Genus::Oak).back()};
    const Pixels full =
        draw(*device, {.anchors = oak, .daylight = {.hours = 15.0F, .longest = 15.4F}});
    const Pixels falling =
        draw(*device, {.anchors = oak, .daylight = {.hours = 10.2F, .longest = 15.4F}});
    CHECK(autumnal_pixels(falling) > 20);
    CHECK(stray_pixels(falling, full) == 0);
}

TEST_CASE("a maple stands narrower than an oak and turns and falls before it", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::flora::Genus;
    const Pixels oak =
        draw(*device, {.anchors = rank_of(1, 0.0, 11.0F, hog::flora::prime_age, Genus::Oak)});
    const Pixels maple =
        draw(*device, {.anchors = rank_of(1, 0.0, 11.0F, hog::flora::prime_age, Genus::Maple)});
    CHECK(leafy_pixels(maple) > 60);
    CHECK(columns_lit(maple) < columns_lit(oak));
    // Early October at 45 degrees north, the maples all colour and the oaks only beginning;
    // by November the maples are bare and the oaks still full.
    const auto maples = rank_of(3, 0.0, 11.0F, hog::flora::prime_age, Genus::Maple);
    const auto oaks = rank_of(3, 0.0, 11.0F, hog::flora::prime_age, Genus::Oak);
    constexpr hog::flora::Daylight october{.hours = 11.2F, .longest = 15.4F};
    constexpr hog::flora::Daylight november{.hours = 10.4F, .longest = 15.4F};
    const Pixels turned = draw(*device, {.anchors = maples, .daylight = october});
    CHECK(autumnal_pixels(turned) > 100);
    const std::size_t green_oaks =
        leafy_pixels(draw(*device, {.anchors = oaks, .daylight = october}));
    CHECK(green_oaks > 50);
    CHECK(10 * leafy_pixels(turned) < green_oaks);
    CHECK(3 * autumnal_pixels(draw(*device, {.anchors = maples, .daylight = november})) <
          autumnal_pixels(draw(*device, {.anchors = oaks, .daylight = november})));
}

TEST_CASE("a palm grows its fronds high on a bare stem, green the year round", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::flora::Genus;
    const Pixels palm =
        draw(*device, {.anchors = rank_of(1, 0.0, 11.0F, hog::flora::prime_age, Genus::Palm)});
    const Pixels oak =
        draw(*device, {.anchors = rank_of(1, 0.0, 11.0F, hog::flora::prime_age, Genus::Oak)});
    CHECK(leafy_pixels(palm) > 30);
    // Its crown all high up, the stem bare beneath it, where an oak's head hangs lower.
    CHECK(lowest_leafy_row(palm) + 3 < lowest_leafy_row(oak));
    // The stem is narrower than a pixel here, but shows.
    CHECK(barky_below(palm, lowest_leafy_row(palm)) > 0);
    // Evergreen.
    const auto palms = rank_of(3, 0.0, 11.0F, hog::flora::prime_age, Genus::Palm);
    constexpr hog::flora::Daylight summer{.hours = 15.0F, .longest = 15.4F};
    constexpr hog::flora::Daylight winter{.hours = 9.0F, .longest = 15.4F, .lengthening = true};
    CHECK(leafy_pixels(draw(*device, {.anchors = palms, .daylight = winter})) ==
          leafy_pixels(draw(*device, {.anchors = palms, .daylight = summer})));
}

TEST_CASE(
    "a dead grown tree stands bare, weathered grey or burned black; a failing one thins and browns",
    "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    using hog::flora::Condition;
    using hog::flora::Genus;
    for (const Genus genus : {Genus::Pine, Genus::Oak, Genus::Maple, Genus::Palm}) {
        INFO("genus " << static_cast<int>(genus));
        const auto trees = rank_of(3, 0.0, 11.0F, hog::flora::prime_age, genus);
        const Pixels sound = draw(*device, {.anchors = trees});
        const Pixels failing = draw(*device, {.anchors = fared(trees, Condition::Failing)});
        const Pixels dead = draw(*device, {.anchors = fared(trees, Condition::Dead)});
        const Pixels charred = draw(*device, {.anchors = fared(trees, Condition::Charred)});
        const Pixels scorched = draw(*device, {.anchors = fared(trees, Condition::Scorched)});
        INFO("bright " << mean_brightness(dead) << " " << mean_brightness(charred));
        // Its crown gone, a dead tree stands as its wood alone, burned darker than weathered.
        CHECK(leafy_pixels(dead) == 0);
        CHECK(leafy_pixels(charred) == 0);
        CHECK(lit_pixels(dead) > 0);
        CHECK(lit_pixels(dead) < lit_pixels(sound) / 2);
        // Some of the charcoal is too dark to light a pixel at all.
        CHECK(lit_pixels(charred) > 0);
        CHECK(lit_pixels(charred) <= lit_pixels(dead));
        CHECK(mean_brightness(charred) < 0.5 * mean_brightness(dead));
        // A failing crown thinner and browner.
        CHECK(lit_pixels(failing) < lit_pixels(sound));
        CHECK(2 * leafy_pixels(failing) < leafy_pixels(sound));
        CHECK(autumnal_pixels(failing) > autumnal_pixels(sound));
        // A scorched survivor's crown untouched, its trunk blackened where it shows.
        CHECK(leafy_pixels(scorched) == leafy_pixels(sound));
        if (barky_pixels(sound) > 4) {
            CHECK(barky_pixels(scorched) < barky_pixels(sound));
        }
    }
}
