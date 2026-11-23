#include "test_device.hpp"

#include <hog/flora/asset.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/trees.hpp>
#include <hog/terrain/bend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <flora_fixture.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::WorldPosition;
using hog::render::TreeDraw;
using hog::render::testing::test_device;

namespace {

constexpr VkExtent2D extent{.width = 256, .height = 256};
constexpr double earth = 6'371'000.0;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};

std::vector<std::byte> fixture() {
    std::vector<std::byte> out(flora_fixture::bytes.size());
    std::ranges::transform(flora_fixture::bytes, out.begin(),
                           [](std::uint8_t b) { return static_cast<std::byte>(b); });
    return out;
}

struct Drawn {
    std::vector<std::uint8_t> pixels;
    hog::render::Trees::Drawn counts;
};

// From two metres over the ground of an Earth-sized planet, looking level east at the tree
// lab's test tree `away` metres off, lit by the sun `sun_elevation` degrees up behind the eye;
// drawn as its meshes to `near` metres, its cards to `cards`, and its impostor from there to a
// kilometre; its leaves dropping `leaves` times sooner than the lab chose.
Drawn look(hog::render::Device& device, double away, double sun_elevation, double near = 500.0,
           double cards = 0.0, double leaves = 1.0) {
    const auto bytes = fixture();
    const auto species = hog::flora::parse_species(bytes);
    REQUIRE(species.has_value());
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(sky);
    auto trees = hog::render::Trees::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(trees);
    const auto added = trees->add(species.value(), 4);
    INFO((added ? std::string{} : added.error().message));
    REQUIRE(added);
    trees->set_reach({near, std::max(near, cards)}, 1000.0);
    trees->set_coarseness(1.0, leaves);
    const WorldPosition eye{up * (earth + 2.0)};
    const double elevation = sun_elevation * std::numbers::pi / 180.0;
    const DVec3 sun = (-east * std::cos(elevation)) + (up * std::sin(elevation));
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, earth, 0.0);
    const hog::render::Camera camera{.eye = eye, .forward = east, .up = up};
    const hog::render::SkyView seen{.camera = camera, .sun = sun, .bend = bend, .exposure = 10.0};
    const hog::render::TreeView view{
        .camera = camera, .sun = sun, .bend = bend, .air = sky->bindings(0, *atmosphere)};
    const std::vector<TreeDraw> drawn{
        {.foot = WorldPosition{(up * earth) + (east * away)},
         .tree = {.species = 0, .variant = 0, .turn = 0.0F, .scale = 1.0F}}};
    trees->show(drawn);
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
        CHECK(trees->draw(commands, extent, view, 0));
        vkCmdEndRendering(commands);
    }));
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return {.pixels = std::move(pixels).value(), .counts = trees->drawn()};
}

struct Covered {
    std::size_t count = 0;
    int brightness = 0;
    // Mean column and row.
    double x = 0.0;
    double y = 0.0;
};

Covered covered(const std::vector<std::uint8_t>& pixels) {
    Covered out;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const int sum = pixels[i] + pixels[i + 1] + pixels[i + 2];
        if (sum > 0) {
            ++out.count;
            out.brightness += sum;
            const std::size_t texel = i / 4;
            out.x += static_cast<double>(texel % extent.width);
            const std::size_t row = texel / extent.width;
            out.y += static_cast<double>(row);
        }
    }
    if (out.count > 0) {
        out.x /= static_cast<double>(out.count);
        out.y /= static_cast<double>(out.count);
    }
    return out;
}

} // namespace

TEST_CASE("a pixel spans metres as the view's height divides it", "[render][trees]") {
    CHECK(std::abs(hog::render::pixel_metres(100.0, std::numbers::pi / 2.0, 200) - 1.0) < 1e-9);
}

TEST_CASE("pieces stay while they drop at a larger pixel", "[render][trees]") {
    const std::array<float, 5> drops{4.0F, 2.0F, 1.0F, 0.5F, 0.25F};
    CHECK(hog::render::kept_of(drops, 10.0F) == 0);
    CHECK(hog::render::kept_of(drops, 1.5F) == 2);
    CHECK(hog::render::kept_of(drops, 1.0F) == 2);
    CHECK(hog::render::kept_of(drops, 0.1F) == 5);
}

TEST_CASE("a tree is drawn where it stands, thinner far off", "[render][gpu][trees]") {
    const auto device = test_device();
    const Drawn near = look(*device, 15.0, 30.0);
    const Covered seen = covered(near.pixels);
    CHECK(near.counts.trees == 1);
    CHECK(seen.count > 8);
    // Its leaves and twigs stand 2 to 18 m up, over the eye's 2 m: above the middle, about it
    // across.
    CAPTURE(seen.x, seen.y);
    CHECK(seen.y < 128.0);
    CHECK(std::abs(seen.x - 128.0) < 64.0);
    const Drawn far = look(*device, 200.0, 30.0);
    CHECK(far.counts.trees == 1);
    CHECK(far.counts.leaves < near.counts.leaves);
    CHECK(covered(far.pixels).count < seen.count);
    const Drawn behind = look(*device, -15.0, 30.0);
    CHECK(behind.counts.trees == 0);
    CHECK(covered(behind.pixels).count == 0);
}

TEST_CASE("leaves can drop out sooner than the wood", "[render][gpu][trees]") {
    const auto device = test_device();
    const Drawn as_baked = look(*device, 4.0, 30.0);
    const Drawn coarser = look(*device, 4.0, 30.0, 500.0, 0.0, 2.0);
    CHECK(coarser.counts.leaves > 0);
    CHECK(coarser.counts.leaves < as_baked.counts.leaves);
    CHECK(coarser.counts.triangles == as_baked.counts.triangles);
}

TEST_CASE("a tree is lit by day and dark by night", "[render][gpu][trees]") {
    const auto device = test_device();
    const Covered day = covered(look(*device, 15.0, 30.0).pixels);
    const Covered night = covered(look(*device, 15.0, -30.0).pixels);
    CHECK(day.brightness > 3 * night.brightness);
}

TEST_CASE("past mesh range a tree is drawn as its impostor, and past that not at all",
          "[render][gpu][trees]") {
    const auto device = test_device();
    const Drawn mid = look(*device, 300.0, 30.0, 120.0);
    CHECK(mid.counts.trees == 0);
    CHECK(mid.counts.impostors == 1);
    const Covered seen = covered(mid.pixels);
    CHECK(seen.count > 20);
    CHECK(std::abs(seen.x - 128.0) < 32.0);
    const Drawn beyond = look(*device, 1500.0, 30.0, 120.0);
    CHECK(covered(beyond.pixels).count == 0);
    const Drawn near = look(*device, 60.0, 30.0, 120.0);
    CHECK(near.counts.trees == 1);
}

TEST_CASE("between meshes and impostors a tree is drawn as its branch cards",
          "[render][gpu][trees]") {
    const auto device = test_device();
    const Drawn carded = look(*device, 60.0, 30.0, 40.0, 100.0);
    CHECK(carded.counts.trees == 1);
    CHECK(carded.counts.carded == 1);
    CHECK(carded.counts.leaves == 0);
    CHECK(carded.counts.triangles == 8); // three squares and the limb
    const Covered seen = covered(carded.pixels);
    CHECK(seen.count > 20);
    CHECK(std::abs(seen.x - 128.0) < 32.0);
    const Drawn whole = look(*device, 30.0, 30.0, 40.0, 100.0);
    CHECK(whole.counts.carded == 0);
    const Drawn far = look(*device, 300.0, 30.0, 40.0, 100.0);
    CHECK(far.counts.trees == 0);
}

TEST_CASE("within a hand-over a tree is drawn at both levels, each taking a share of its pixels",
          "[render][gpu][trees]") {
    const auto device = test_device();
    const Drawn handing = look(*device, 40.0, 30.0, 40.0, 100.0);
    CHECK(handing.counts.trees == 1);
    CHECK(handing.counts.carded == 1);
    CHECK(handing.counts.triangles > 8); // the meshes' and the cards'
    CHECK(covered(handing.pixels).count > 20);
    const Drawn past = look(*device, 45.0, 30.0, 40.0, 100.0);
    CHECK(past.counts.carded == 1);
    CHECK(past.counts.triangles == 8);
}
