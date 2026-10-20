#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/flora/species.hpp>
#include <hog/render/anchors.hpp>
#include <hog/render/atmosphere.hpp>
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
using hog::render::GrassView;
using hog::render::StandDraw;
using hog::render::TreeAnchor;
using hog::render::Trees;
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
    double hold_off = 0.0;
    DVec3 wind{0.0};
    double seconds = 0.0;
    DVec3 eye_back{0.0, -40.0, 6.0}; // east, north, up of the tile's origin
    hog::flora::Daylight daylight{};
};

// The stand's stamps alone over black, from south of the origin facing north.
Pixels draw(hog::render::Device& device, const Stand& stand) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, colour_format, depth_format);
    REQUIRE(sky);
    auto trees = Trees::create(device, colour_format, depth_format, stand.hold_off);
    REQUIRE(trees);
    auto pool = AnchorPool::create(device, {.tiles = 1, .frames = 1, .uploads = 1});
    REQUIRE(pool);
    const std::uint32_t slot = pool->acquire().value_or(99);
    REQUIRE(slot == 0);
    REQUIRE(pool->stage(0, slot, stand.anchors));

    const WorldPosition origin{up * (radius + ground_height)};
    const WorldPosition eye{(up * (radius + ground_height + stand.eye_back.z)) +
                            (east * stand.eye_back.x) + (north * stand.eye_back.y)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, ground_height);
    const double elevation = 40.0 * std::numbers::pi / 180.0;
    const DVec3 sun = (east * std::cos(elevation)) + (up * std::sin(elevation));
    const hog::render::Camera camera{.eye = eye, .forward = north, .up = up, .vertical_fov = 0.6};
    const hog::atmosphere::Model model;
    const double scale = hog::render::air_scale(model.air(), bend);
    const hog::render::SkyView seen{.camera = camera,
                                    .sun = sun,
                                    .bend = bend,
                                    .exposure = 10.0,
                                    .sky_light = model.sky_irradiance(2.0 * scale, sun),
                                    .daylight = stand.daylight};
    const StandDraw stamps{.slot = slot,
                           .count = static_cast<std::uint32_t>(stand.anchors.size()),
                           .origin = origin,
                           .detail = 0};
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
        trees->draw(commands, extent, *pool, {&stamps, 1}, view);
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

TEST_CASE("foliage packs into a word with its genus, dark shades kept", "[render]") {
    using hog::flora::Genus;
    const Vec3 dark{0.02F, 0.06F, 0.03F};
    const std::uint32_t packed = hog::render::packed_foliage(dark, Genus::Oak);
    const Vec3 back = hog::render::foliage_of(packed);
    for (glm::length_t channel = 0; channel < 3; ++channel) {
        CHECK(std::abs(back[channel] - dark[channel]) < dark[channel] * 0.05F);
    }
    CHECK(hog::render::genus_of(packed) == Genus::Oak);
    CHECK(hog::render::genus_of(hog::render::packed_foliage(dark, Genus::Pine)) == Genus::Pine);
    // Out of range clamps rather than spilling into the next channel; an unknown genus is a pine.
    const std::uint32_t bright = hog::render::packed_foliage(Vec3{2.0F, -1.0F, 0.5F}, Genus::Oak);
    CHECK(hog::render::foliage_of(bright).r == 1.0F);
    CHECK(hog::render::foliage_of(bright).g == 0.0F);
    CHECK(hog::render::genus_of(bright) == Genus::Oak);
    CHECK(hog::render::genus_of(0xff00'0000U) == Genus::Pine);
    // The tree's condition beside its genus, neither spilling into the other.
    using hog::flora::Condition;
    CHECK(hog::render::condition_of(packed) == Condition::Sound);
    for (const Condition condition :
         {Condition::Failing, Condition::Scorched, Condition::Dead, Condition::Charred}) {
        const std::uint32_t fared = hog::render::packed_foliage(dark, Genus::Palm, condition);
        CHECK(hog::render::condition_of(fared) == condition);
        CHECK(hog::render::genus_of(fared) == Genus::Palm);
        CHECK(hog::render::foliage_of(fared) == hog::render::foliage_of(packed));
    }
    CHECK(hog::render::condition_of(0xf000'0000U) == Condition::Sound);
}

TEST_CASE("a stamp stands on its anchor, trunk under a tiered crown", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const Pixels stand = draw(*device, {.anchors = rank_of(5, 0.0)});
    CHECK(lit_pixels(stand) > 150);
    CHECK(barky_pixels(stand) > 2);
    // An empty tile draws nothing.
    CHECK(lit_pixels(draw(*device, {.anchors = {}})) == 0);
}

TEST_CASE("stamps hold off where the grown forest covers, and shrink away far off", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    // The same rank 100 m out: held off while the forest's reach covers it, stood without.
    const Stand near{.anchors = rank_of(5, 100.0)};
    CHECK(lit_pixels(draw(*device, near)) > 30);
    const Stand held{.anchors = rank_of(5, 100.0), .hold_off = 240.0};
    CHECK(lit_pixels(draw(*device, held)) == 0);
    // Beyond their own reach, the stamps have shrunk away entirely.
    const Stand gone{.anchors = rank_of(5, 950.0)};
    CHECK(lit_pixels(draw(*device, gone)) == 0);
}

TEST_CASE("the wind rocks the stamps' tops, and still air leaves them still", "[render]") {
    auto device = test_device();
    if (!device) {
        SKIP("no Vulkan device");
    }
    const DVec3 wind{14.0, 0.0, 0.0};
    const Stand windy{.anchors = rank_of(5, 0.0), .wind = wind, .seconds = 0.0};
    Stand later = windy;
    later.seconds = 1.1;
    const Pixels one = draw(*device, windy);
    const Pixels two = draw(*device, later);
    CHECK(one != two);
    const Stand still_stand{.anchors = rank_of(5, 0.0)};
    Stand still_later = still_stand;
    still_later.seconds = 1.1;
    CHECK(draw(*device, still_stand) == draw(*device, still_later));
}

TEST_CASE("each stamp stands as its age: saplings green to the ground, old trees bare below",
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

TEST_CASE("an oak's stamp is a broad round head where a pine's of its height stands narrow",
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
    CHECK(barky_pixels(oak) > 0);
    CHECK(columns_lit(oak) > columns_lit(pine) + (columns_lit(pine) / 2));
    CHECK(leafy_pixels(oak) > leafy_pixels(pine));
}

TEST_CASE("an oak's stamp turns in autumn and stands as bare twigs in winter", "[render]") {
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
    // Twigs thin out the crown's whole head, with the sky through it.
    CHECK(lit_pixels(bare) > lit_pixels(green) / 8);
    CHECK(lit_pixels(bare) < lit_pixels(green) / 2);

    const auto pines = rank_of(3, 0.0);
    CHECK(leafy_pixels(draw(*device, {.anchors = pines, .daylight = winter})) ==
          leafy_pixels(draw(*device, {.anchors = pines, .daylight = summer})));
}

TEST_CASE("a maple's stamp stands narrower than an oak and turns and falls before it", "[render]") {
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

TEST_CASE("a palm's stamp is a crown of fronds high on a bare stem, green the year round",
          "[render]") {
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
    CHECK(barky_below(palm, lowest_leafy_row(palm)) > 3);
    // Evergreen.
    const auto palms = rank_of(3, 0.0, 11.0F, hog::flora::prime_age, Genus::Palm);
    constexpr hog::flora::Daylight summer{.hours = 15.0F, .longest = 15.4F};
    constexpr hog::flora::Daylight winter{.hours = 9.0F, .longest = 15.4F, .lengthening = true};
    CHECK(leafy_pixels(draw(*device, {.anchors = palms, .daylight = winter})) ==
          leafy_pixels(draw(*device, {.anchors = palms, .daylight = summer})));
}

TEST_CASE(
    "a dead stamp stands bare, weathered grey or burned black; a failing one thins and browns",
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
