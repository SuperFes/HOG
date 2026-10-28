#include "test_device.hpp"

#include <hog/core/math.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/capsules.hpp>
#include <hog/render/device.hpp>
#include <hog/render/drifting_grid.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/ripples.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/shore.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec2;
using hog::DVec3;
using hog::render::GridStep;
using hog::render::ripple_calming;
using hog::render::ripple_settling;
using hog::render::ripple_size;
using hog::render::ripple_spacing;
using hog::render::RippleBody;
using hog::render::testing::test_device;

namespace {

constexpr double gravity = 9.81;
constexpr std::size_t texels = std::size_t{ripple_size} * ripple_size;

std::size_t index_of(int x, int y) {
    return (static_cast<std::size_t>(y) * ripple_size) + static_cast<std::size_t>(x);
}

// Each texel the map keeps: how high the water stands there, and its potential.
std::vector<DVec2> kept_texels(hog::render::Device& device, const hog::render::RippleMap& map) {
    auto pixels = hog::render::read_pixels(device, map.kept(), hog::render::image_use::sampled);
    REQUIRE(pixels);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        hog::render::transition(commands, map.kept().handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                hog::render::image_use::copy_source,
                                hog::render::image_use::sampled);
    }));
    std::vector<glm::vec2> kept(texels);
    REQUIRE(pixels->size() == std::span{kept}.size_bytes());
    std::memcpy(kept.data(), pixels->data(), pixels->size());
    return {kept.begin(), kept.end()};
}

// Lays `ripples` on what `map` keeps, a row at a time.
void lay_kept(hog::render::Device& device, const hog::render::RippleMap& map,
              std::span<const DVec2> ripples) {
    const std::vector<glm::vec2> laid{ripples.begin(), ripples.end()};
    auto staging = hog::render::make_filled_buffer(device, std::as_bytes(std::span{laid}),
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(staging);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        VkImage kept = map.kept().handle();
        hog::render::transition(commands, kept, VK_IMAGE_ASPECT_COLOR_BIT,
                                hog::render::image_use::sampled,
                                hog::render::image_use::copy_target);
        const VkBufferImageCopy copy{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageOffset = {.x = 0, .y = 0, .z = 0},
            .imageExtent = {.width = ripple_size, .height = ripple_size, .depth = 1}};
        vkCmdCopyBufferToImage(commands, staging->handle(), kept,
                               hog::render::image_use::copy_target.layout, 1, &copy);
        hog::render::transition(commands, kept, VK_IMAGE_ASPECT_COLOR_BIT,
                                hog::render::image_use::copy_target,
                                hog::render::image_use::sampled);
    }));
}

// A train of ripples `high` metres high, turning through `wavenumber` rad/m along the grid,
// running that way on deep water, `seconds` after its crest crossed the first texel.
std::vector<DVec2> train(DVec2 wavenumber, double high, double seconds) {
    const double k = glm::length(wavenumber);
    const double omega = std::sqrt(gravity * k);
    std::vector<DVec2> ripples;
    ripples.reserve(texels);
    for (std::uint32_t y = 0; y < ripple_size; ++y) {
        for (std::uint32_t x = 0; x < ripple_size; ++x) {
            const double phase =
                glm::dot(wavenumber, DVec2{x, y} * ripple_spacing) - (omega * seconds);
            ripples.emplace_back(high * std::cos(phase), high * omega / k * std::sin(phase));
        }
    }
    return ripples;
}

// A mound of water `high` metres high at the grid's middle, spread over about `wide` metres.
std::vector<DVec2> mound(double high, double wide) {
    std::vector<DVec2> ripples;
    ripples.reserve(texels);
    const DVec2 middle{ripple_size / 2.0};
    for (std::uint32_t y = 0; y < ripple_size; ++y) {
        for (std::uint32_t x = 0; x < ripple_size; ++x) {
            const double across = glm::distance(DVec2{x, y}, middle) * ripple_spacing / wide;
            ripples.emplace_back(high * std::exp(-across * across), 0.0);
        }
    }
    return ripples;
}

double squared_height(std::span<const DVec2> ripples) {
    double sum = 0.0;
    for (const DVec2 ripple : ripples) {
        sum += ripple.x * ripple.x;
    }
    return sum;
}

// Of wavenumbers from `lowest` to `highest`, rad/m, the one `heights` along a line of texels rise
// and fall by most.
double strongest(std::span<const double> heights, double lowest, double highest) {
    double best = lowest;
    double most = -1.0;
    const int tries = static_cast<int>(std::ceil(std::log(highest / lowest) / std::log(1.002)));
    for (int tried = 0; tried <= tries; ++tried) {
        const double wavenumber = lowest * std::pow(1.002, tried);
        std::complex<double> sum{0.0};
        for (std::size_t i = 0; i < heights.size(); ++i) {
            // Hann windowed, so the line's ends don't blur the peak.
            const double windowing =
                0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                      static_cast<double>(heights.size() - 1)));
            sum += heights[i] * windowing *
                   std::polar(1.0, -wavenumber * static_cast<double>(i) * ripple_spacing);
        }
        if (std::abs(sum) > most) {
            most = std::abs(sum);
            best = wavenumber;
        }
    }
    return best;
}

constexpr VkExtent2D seen_extent{.width = 64, .height = 64};

// Of a picture of seen_extent, how far from its middle the furthest pixel the sun glints off lies,
// pixels: that's brighter red than `bright`.
double glint_reach(std::span<const std::uint8_t> pixels, int bright) {
    double reach = 0.0;
    for (std::uint32_t y = 0; y < seen_extent.height; ++y) {
        for (std::uint32_t x = 0; x < seen_extent.width; ++x) {
            const std::size_t at = ((std::size_t{y} * seen_extent.width) + x) * 4;
            if (std::cmp_greater(pixels[at], bright)) {
                reach = std::max(
                    reach, glm::distance(DVec2{x, y} + 0.5,
                                         DVec2{seen_extent.width / 2.0, seen_extent.height / 2.0}));
            }
        }
    }
    return reach;
}

// The calm sea seen straight down from `height` metres, the sun overhead, with `ripples` laid
// round the eye if any, and `rain` as DrawnWaves has it; read back.
std::vector<std::uint8_t> seen_down(hog::render::Device& device, double height,
                                    const std::optional<std::vector<DVec2>>& ripples,
                                    glm::vec4 rain = glm::vec4{0.0F}) {
    auto ocean = hog::render::Ocean::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(ocean);
    constexpr double radius = 115'000.0;
    const DVec3 up{0.0, 0.0, 1.0};
    const hog::WorldPosition eye{up * (radius + height)};
    const hog::render::OceanView view{
        .camera = {.eye = eye, .forward = -up, .up = DVec3{0.0, 1.0, 0.0}},
        .sun = up,
        .sky = {0.2F, 0.4F, 0.8F},
        .bend = hog::terrain::bend_from(eye, radius, 0.0),
        .waves = {.rain = rain}};
    if (ripples) {
        REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
            REQUIRE(ocean->step_ripples(
                commands, view, 0, hog::GameTime{std::chrono::seconds{1'792'000'000}}, DVec3{0.0}));
        }));
        lay_kept(device, ocean->ripples(), *ripples);
    }
    auto target = hog::render::make_image(
        device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                 .extent = seen_extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(target);
    auto depth =
        hog::render::make_image(device, {.format = VK_FORMAT_D32_SFLOAT,
                                         .extent = seen_extent,
                                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
    REQUIRE(depth);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, target->view(), seen_extent,
                                     VkClearColorValue{.float32 = {0.0F, 0.0F, 0.0F, 1.0F}},
                                     {.view = depth->view()});
        CHECK(ocean->draw(commands, seen_extent, view));
        vkCmdEndRendering(commands);
    }));
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

using hog::render::rain_tile_cells;
using hog::render::rain_tile_layers;
using hog::render::rain_tile_size;

constexpr std::size_t rain_texels = std::size_t{rain_tile_size} * rain_tile_size;

// Of each layer of what `tile` has drawn, how steeply the rings slope each texel; read back.
std::vector<DVec2> drawn_rings(hog::render::Device& device, const hog::render::RainTile& tile) {
    const std::size_t count = rain_texels * rain_tile_layers;
    auto buffer =
        hog::render::make_buffer(device, count * sizeof(glm::vec2),
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, hog::render::Memory::Readback);
    REQUIRE(buffer);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, tile.kept().handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                use::sampled, use::copy_source);
        const VkBufferImageCopy copy{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = rain_tile_layers},
            .imageOffset = {.x = 0, .y = 0, .z = 0},
            .imageExtent = {.width = rain_tile_size, .height = rain_tile_size, .depth = 1}};
        vkCmdCopyImageToBuffer(commands, tile.kept().handle(), use::copy_source.layout,
                               buffer->handle(), 1, &copy);
        hog::render::transition(commands, tile.kept().handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                use::copy_source, use::sampled);
    }));
    REQUIRE(buffer->invalidate());
    std::vector<glm::vec2> drawn(count);
    std::memcpy(drawn.data(), buffer->mapped().data(), std::span{drawn}.size_bytes());
    return {drawn.begin(), drawn.end()};
}

// As rain_rings.slang's random_of.
glm::dvec3 random_of(glm::ivec3 seed) {
    glm::uvec3 v = (glm::uvec3(seed) * 1664525U) + 1013904223U;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16U;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return glm::dvec3(v) / 4294967296.0;
}

// How steeply the rings of the tile's layer `layer` slope the water at `at`, cells from its
// corner, inside it or past its edges, with `rain` as DrawnWaves has it: worked out as
// rain_rings.slang draws them.
DVec2 rings_at(DVec2 at, int layer, glm::dvec2 rain) {
    using hog::render::rain_ring_cell;
    using hog::render::rain_ring_layers;
    using hog::render::rain_ring_life;
    constexpr int each_layers = rain_ring_layers / static_cast<int>(rain_tile_layers);
    constexpr double speed = 0.3;
    constexpr double length = 0.03;
    constexpr double steepest = 0.3;
    DVec2 slope{0.0};
    for (int each = 0; each < each_layers; ++each) {
        const int ringing = (layer * each_layers) + each;
        const DVec2 within = at + (DVec2{0.37, 0.71} * static_cast<double>(ringing));
        const glm::ivec2 cell{glm::floor(within)};
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                const glm::ivec2 near = cell + glm::ivec2{x, y};
                const glm::ivec2 seed =
                    ((near % rain_tile_cells) + rain_tile_cells) % rain_tile_cells;
                const double lived = (rain.y / rain_ring_life) +
                                     random_of(glm::ivec3{seed, ringing - rain_ring_layers}).x;
                const double age = lived - std::floor(lived);
                const glm::dvec3 drop = random_of(glm::ivec3{
                    seed, (static_cast<int>(std::floor(lived)) * rain_ring_layers) + ringing});
                if (drop.z >= rain.x) {
                    continue;
                }
                const DVec2 from = (within - (DVec2{near} + DVec2{drop})) * rain_ring_cell;
                const double out = glm::length(from);
                const double behind = out - (age * rain_ring_life * speed);
                if (std::abs(behind) > 2.0 * length) {
                    continue;
                }
                const double across = behind / length;
                const double steepness = -steepest * (1.0 - age) * (1.0 - age) *
                                         std::exp(-across * across) *
                                         std::sin(2.0 * std::numbers::pi * across);
                slope += from * (steepness / std::max(out, 1e-4));
            }
        }
    }
    return slope;
}

// Cells from the tile's corner to the middle of the texel at `x`, `y`.
DVec2 rain_texel_at(int x, int y) {
    return (DVec2{x, y} + 0.5) * (static_cast<double>(rain_tile_cells) / rain_tile_size);
}

} // namespace

TEST_CASE("ripples run as fast as waves on deep water, short or long, any way", "[render][gpu]") {
    const auto device = test_device();
    auto map = hog::render::RippleMap::create(*device);
    REQUIRE(map);
    auto calm = hog::render::CalmSea::create(*device);
    REQUIRE(calm);
    // Each a whole number of turns across the grid.
    const double turn = 2.0 * std::numbers::pi / (ripple_size * ripple_spacing);
    const double high = 0.02;
    const double seconds = 0.5;
    for (const DVec2 wavenumber :
         {DVec2{64.0 * turn, 0.0}, DVec2{0.0, 8.0 * turn}, DVec2{8.0 * turn, 8.0 * turn},
          DVec2{-3.0 * turn, 20.0 * turn}}) {
        lay_kept(*device, *map, train(wavenumber, high, 0.0));
        map->stir();
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(map->step(commands, {.shift = {0, 0}, .seconds = seconds, .kept = true},
                              calm->bindings()));
        }));
        const std::vector<DVec2> ran = kept_texels(*device, *map);
        const std::vector<DVec2> expected = train(wavenumber, high, seconds);
        // Away from the edge, where they're soaked up.
        double worst = 0.0;
        for (int y = 128; y < 384; y += 7) {
            for (int x = 128; x < 384; x += 5) {
                worst = std::max(
                    worst, std::abs(ran.at(index_of(x, y)).x - expected.at(index_of(x, y)).x));
            }
        }
        CHECK_THAT(worst, WithinAbs(0.0, high * 0.01));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("ripples run off the grid's edge rather than back onto it from the other side",
          "[render][gpu]") {
    const auto device = test_device();
    auto map = hog::render::RippleMap::create(*device);
    REQUIRE(map);
    auto calm = hog::render::CalmSea::create(*device);
    REQUIRE(calm);
    const std::vector<DVec2> laid = mound(0.05, 2.0);
    lay_kept(*device, *map, laid);
    const double at_first = squared_height(laid);
    double most = 0.0;
    std::vector<DVec2> ran;
    for (int second = 0; second < 60; ++second) {
        // However long they run, they're soaked up rather than left still.
        map->stir();
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(map->step(commands, {.shift = {0, 0}, .seconds = 1.0, .kept = true},
                              calm->bindings()));
        }));
        ran = kept_texels(*device, *map);
        most = std::max(most, squared_height(ran));
    }
    CHECK(most <= at_first * 1.01);
    CHECK(squared_height(ran) < at_first * 0.01);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("ripples stay on the water as the grid steps under them, and none are kept where nothing "
          "is",
          "[render][gpu]") {
    const auto device = test_device();
    auto map = hog::render::RippleMap::create(*device);
    REQUIRE(map);
    auto calm = hog::render::CalmSea::create(*device);
    REQUIRE(calm);
    std::vector<DVec2> laid;
    laid.reserve(texels);
    for (std::uint32_t y = 0; y < ripple_size; ++y) {
        for (std::uint32_t x = 0; x < ripple_size; ++x) {
            laid.emplace_back(std::sin(x * 0.05) * std::cos(y * 0.03) * 0.01,
                              std::cos(x * 0.02) * 0.01);
        }
    }
    lay_kept(*device, *map, laid);
    map->stir();
    const auto stepped = [&](const GridStep& step) {
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(map->step(commands, step, calm->bindings()));
        }));
        return kept_texels(*device, *map);
    };
    const glm::ivec2 shift{3, -2};
    const std::vector<DVec2> shifted = stepped({.shift = shift, .seconds = 0.0, .kept = true});
    for (const glm::ivec2 texel :
         {glm::ivec2{100, 300}, glm::ivec2{256, 256}, glm::ivec2{400, 60}}) {
        const DVec2 now = shifted.at(index_of(texel.x, texel.y));
        const DVec2 before = laid.at(index_of(texel.x + shift.x, texel.y + shift.y));
        CHECK_THAT(glm::distance(now, before), WithinAbs(0.0, 1e-6));
    }
    // What lay beyond the grid's edge before it stepped isn't known.
    CHECK(glm::length(shifted.at(index_of(ripple_size - 1, 100))) < 1e-6);
    CHECK(glm::length(shifted.at(index_of(100, 0))) < 1e-6);
    const std::vector<DVec2> none = stepped({.shift = {0, 0}, .seconds = 0.1, .kept = false});
    CHECK(std::ranges::all_of(none, [](DVec2 texel) { return texel == DVec2{0.0}; }));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("ripples stop at the shore", "[render][gpu]") {
    const auto device = test_device();
    auto ocean =
        hog::render::Ocean::create(*device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(ocean);
    constexpr double radius = 115'000.0;
    const DVec3 up = glm::normalize(DVec3{0.2, -0.1, 1.0});
    hog::render::OceanView view;
    view.bend = hog::terrain::bend_from(hog::WorldPosition{up * (radius + 2.0)}, radius, 0.0);
    // Level ground all round, `depth` metres under the sea's mean.
    const auto shore_of = [&](float depth) {
        const std::size_t shore_texels = hog::terrain::shore_spacing.size() *
                                         hog::terrain::shore_size * hog::terrain::shore_size;
        return hog::terrain::ShoreDepths{
            .frame = hog::terrain::shore_frame(view.bend.up, radius),
            .depths = std::vector<float>(shore_texels, depth),
            .still = std::vector<float>(shore_texels, hog::terrain::shore_highest),
            .levels = std::vector<float>(shore_texels, 0.0F),
            .fetches = std::vector<float>(shore_texels * hog::terrain::fetch_headings, 0.0F),
            .offshore = {}};
    };
    hog::GameTime time{std::chrono::seconds{1'792'000'000}};
    const std::vector<DVec2> laid = mound(0.05, 2.0);
    const auto stepped = [&](float depth) {
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(ocean->update_shore(commands, 0, shore_of(depth)));
            REQUIRE(ocean->step_ripples(commands, view, 0, time, DVec3{0.0}));
        }));
        lay_kept(*device, ocean->ripples(), laid);
        ocean->ripples().stir();
        time += std::chrono::milliseconds{100};
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(ocean->step_ripples(commands, view, 0, time, DVec3{0.0}));
        }));
        return kept_texels(*device, ocean->ripples());
    };
    CHECK(squared_height(stepped(2.0F)) > squared_height(laid) * 0.5);
    CHECK(squared_height(stepped(-0.5F)) == 0.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a body wading through the water leaves a wake behind it, its crests as far apart as "
          "waves running as fast as it",
          "[render][gpu]") {
    const auto device = test_device();
    auto map = hog::render::RippleMap::create(*device);
    REQUIRE(map);
    auto calm = hog::render::CalmSea::create(*device);
    REQUIRE(calm);
    const double speed = 2.0;
    const double frame = 1.0 / 30.0;
    RippleBody body{.feet = {100.0F, 256.5F}, .above = -1.0F};
    for (int i = 0; i < 360; ++i) {
        body.feet.x = static_cast<float>(100.0 + (speed * frame * i / ripple_spacing));
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(map->step(commands, {.shift = {0, 0}, .seconds = frame, .kept = true},
                              calm->bindings(), 0, std::span{&body, 1}));
        }));
    }
    const std::vector<DVec2> ran = kept_texels(*device, *map);
    // Along its track, past where it pushes the water down, the waves across it.
    const int at = static_cast<int>(body.feet.x);
    std::vector<double> behind;
    for (int x = at - 128; x < at - 16; ++x) {
        behind.push_back(ran.at(index_of(x, 256)).x);
    }
    const double wavenumber = gravity / (speed * speed);
    CHECK_THAT(strongest(behind, wavenumber * 0.5, wavenumber * 2.0) / wavenumber,
               WithinAbs(1.0, 0.03));
    double most_behind = 0.0;
    for (const double height : behind) {
        most_behind = std::max(most_behind, std::abs(height));
    }
    CHECK(most_behind > 0.005);
    double most_ahead = 0.0;
    for (int x = at + 24; x < at + 64; ++x) {
        most_ahead = std::max(most_ahead, std::abs(ran.at(index_of(x, 256)).x));
    }
    CHECK(most_ahead < most_behind * 0.2);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a body out of the water leaves it still", "[render][gpu]") {
    const auto device = test_device();
    auto map = hog::render::RippleMap::create(*device);
    REQUIRE(map);
    auto calm = hog::render::CalmSea::create(*device);
    REQUIRE(calm);
    RippleBody body{.feet = {256.0F, 256.0F}, .above = 0.5F};
    for (int i = 0; i < 4; ++i) {
        body.feet.x += 2.0F;
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(map->step(commands, {.shift = {0, 0}, .seconds = 0.1, .kept = true},
                              calm->bindings(), 0, std::span{&body, 1}));
        }));
    }
    const std::vector<DVec2> ran = kept_texels(*device, *map);
    CHECK(std::ranges::all_of(ran, [](DVec2 texel) { return texel == DVec2{0.0}; }));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("bodies are laid on the ripples' grid where they stand, the nearest the eye first",
          "[render]") {
    constexpr double radius = 115'000.0;
    constexpr double height = 2.0;
    const DVec3 up = glm::normalize(DVec3{0.2, -0.1, 1.0});
    const hog::terrain::Bend bend =
        hog::terrain::bend_from(hog::WorldPosition{up * (radius + height)}, radius, 0.0);
    hog::render::DriftingGrid grid{ripple_size, ripple_spacing};
    (void)grid.follow(up, radius, DVec3{0.0}, 0.0);
    const hog::render::GridPlacing placing = grid.placing(up, radius, height);
    const hog::planet::TangentFrame ground = hog::planet::tangent_frame(up);
    // Upright, `along` metres east and north of under the eye, its feet `above` sea level.
    const auto standing = [&](DVec2 along, double above) {
        const DVec3 at =
            glm::normalize((up * radius) + (ground.east * along.x) + (ground.north * along.y));
        return hog::render::Capsule{.feet = hog::WorldPosition{at * (radius + above)}, .up = at};
    };
    std::vector<hog::render::Capsule> capsules{standing({2.0, 1.0}, -1.0),
                                               standing({100.0, 0.0}, 0.0)};
    for (int i = 0; i < 20; ++i) {
        capsules.push_back(standing({-5.0 - i, 0.0}, 0.0));
    }
    const std::vector<RippleBody> bodies = hog::render::ripple_bodies(capsules, placing, bend);
    REQUIRE(bodies.size() == hog::render::ripple_most_bodies);
    const DVec2 under = hog::render::grid_texel(placing, -up * height);
    const RippleBody& wading = bodies.front();
    CHECK_THAT(glm::distance(DVec2{wading.feet}, under + DVec2{16.0, 8.0}), WithinAbs(0.0, 1e-2));
    CHECK_THAT(wading.above, WithinAbs(-1.0, 1e-3));
    CHECK_THAT(glm::distance(wading.tall, glm::vec3{0.0F, 0.0F, 1.8F}), WithinAbs(0.0, 1e-3));
    // The one off the grid is left out, as are the furthest of the rest.
    CHECK(
        std::ranges::none_of(bodies, [](const RippleBody& body) { return body.feet.x > 400.0F; }));
    CHECK_THAT(bodies.back().feet.x, WithinAbs(under.x - (19.0 / ripple_spacing), 1e-2));
}

TEST_CASE("a body pushes the sea where it crosses the surface, not where it's wholly under it",
          "[render][gpu]") {
    const auto device = test_device();
    auto ocean =
        hog::render::Ocean::create(*device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(ocean);
    constexpr double radius = 115'000.0;
    constexpr double height = 2.0;
    const DVec3 up = glm::normalize(DVec3{0.2, -0.1, 1.0});
    hog::render::OceanView view;
    view.bend = hog::terrain::bend_from(hog::WorldPosition{up * (radius + height)}, radius, 0.0);
    const std::size_t shore_texels =
        hog::terrain::shore_spacing.size() * hog::terrain::shore_size * hog::terrain::shore_size;
    const hog::terrain::ShoreDepths shore{
        .frame = hog::terrain::shore_frame(view.bend.up, radius),
        .depths = std::vector<float>(shore_texels, 4.0F),
        .still = std::vector<float>(shore_texels, hog::terrain::shore_highest),
        .levels = std::vector<float>(shore_texels, 0.0F),
        .fetches = std::vector<float>(shore_texels * hog::terrain::fetch_headings, 0.0F),
        .offshore = {}};
    const hog::planet::TangentFrame ground = hog::planet::tangent_frame(up);
    const DVec3 at = glm::normalize((up * radius) + (ground.east * 2.0) + (ground.north * 1.0));
    hog::GameTime time{std::chrono::seconds{1'792'000'000}};
    // Stood `above` sea level, 2 m east and 1 m north of under the eye, stepped on 0.1 s.
    const auto pushed_by = [&](double above) {
        const hog::render::Capsule body{.feet = hog::WorldPosition{at * (radius + above)},
                                        .up = at};
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(ocean->update_shore(commands, 0, shore));
            REQUIRE(ocean->step_ripples(commands, view, 0, time, DVec3{0.0}));
        }));
        time += std::chrono::milliseconds{100};
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(ocean->step_ripples(commands, view, 0, time, DVec3{0.0}, std::span{&body, 1}));
        }));
        // Forgotten before the next, by stepping too long.
        time += std::chrono::seconds{10};
        return kept_texels(*device, ocean->ripples());
    };
    const std::vector<DVec2> wading = pushed_by(-1.0);
    // The grid's centred under the eye to within half a texel.
    double near = 0.0;
    for (int y = 256 + 8 - 2; y <= 256 + 8 + 2; ++y) {
        for (int x = 256 + 16 - 2; x <= 256 + 16 + 2; ++x) {
            near = std::min(near, wading.at(index_of(x, y)).x);
        }
    }
    CHECK(near < -0.002);
    CHECK(std::abs(wading.at(index_of(256 - 80, 256)).x) < 1e-5);
    const std::vector<DVec2> under = pushed_by(-3.0);
    CHECK(std::ranges::all_of(under, [](DVec2 texel) { return texel == DVec2{0.0}; }));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("ripples scatter the sun's glint off the sea, or blur it where too fine to draw",
          "[render][gpu]") {
    const auto device = test_device();
    const std::vector<DVec2> ripples = train({8.0, 3.0}, 0.01, 0.0);
    const std::vector<std::uint8_t> calm = seen_down(*device, 4.0, std::nullopt);
    const std::vector<std::uint8_t> rippled = seen_down(*device, 4.0, ripples);
    CAPTURE(glint_reach(calm, 100), glint_reach(rippled, 100));
    CHECK(glint_reach(rippled, 100) > 2.0 * glint_reach(calm, 100));
    // From high over them, too fine to draw, they only blur it.
    const std::vector<std::uint8_t> calm_far = seen_down(*device, 200.0, std::nullopt);
    const std::vector<std::uint8_t> rippled_far = seen_down(*device, 200.0, ripples);
    CAPTURE(glint_reach(calm_far, 20), glint_reach(rippled_far, 20));
    CHECK(glint_reach(rippled_far, 20) > 1.2 * glint_reach(calm_far, 20));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("rain rings the water as often as drops big enough fall, not snow", "[render]") {
    using hog::render::rain_ring_chance;
    CHECK(rain_ring_chance(0.0, 0.0) == 0.0);
    CHECK(rain_ring_chance(4.0, 1.0) == 0.0);
    CHECK_THAT(rain_ring_chance(2.0, 0.5), WithinAbs(rain_ring_chance(1.0, 0.0), 1e-12));
    // Each cell of each layer rings once a life as often as so many drops fall on it.
    const double cell = hog::render::rain_ring_cell * hog::render::rain_ring_cell;
    CHECK_THAT(rain_ring_chance(1.0, 0.0) * hog::render::rain_ring_layers /
                   (cell * hog::render::rain_ring_life),
               WithinAbs(hog::render::rings_per_rain, 1e-9));
    CHECK(rain_ring_chance(200.0, 0.0) == 1.0);
    // Counted round, but on to the next second the same.
    const hog::GameTime now{std::chrono::seconds{1'792'000'000}};
    const double seconds = hog::render::rain_ring_seconds(now);
    CHECK(seconds >= 0.0);
    CHECK(seconds < hog::render::rain_ring_life * 5'000.0);
    CHECK_THAT(std::fmod(hog::render::rain_ring_seconds(now + std::chrono::milliseconds{250}) -
                             seconds + 3'500.0,
                         3'500.0),
               WithinAbs(0.25, 1e-6));
}

TEST_CASE("rain rings scatter the sun's glint off the sea, or blur it where too fine to draw",
          "[render][gpu]") {
    const auto device = test_device();
    const std::vector<DVec2> still(texels);
    const glm::vec4 rain{static_cast<float>(hog::render::rain_ring_chance(5.0, 0.0)), 12.3F, 0.0F,
                         0.0F};
    const std::vector<std::uint8_t> dry = seen_down(*device, 0.5, still);
    const std::vector<std::uint8_t> wet = seen_down(*device, 0.5, still, rain);
    CAPTURE(glint_reach(dry, 100), glint_reach(wet, 100));
    CHECK(glint_reach(wet, 100) > 2.0 * glint_reach(dry, 100));
    const std::vector<std::uint8_t> dry_far = seen_down(*device, 200.0, still);
    const std::vector<std::uint8_t> wet_far = seen_down(*device, 200.0, still, rain);
    // Dimmer, so seen fainter.
    CAPTURE(glint_reach(dry_far, 5), glint_reach(wet_far, 5));
    CHECK(glint_reach(wet_far, 5) > 1.5 * glint_reach(dry_far, 5));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("rain's rings are drawn on a tile that repeats seamlessly, as many as rain falls",
          "[render][gpu]") {
    const auto device = test_device();
    auto tile = hog::render::RainTile::create(*device);
    REQUIRE(tile);
    const auto drawn_with = [&](double chance) {
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            tile->draw(commands, {static_cast<float>(chance), 12.3F, 0.0F, 0.0F});
        }));
        return drawn_rings(*device, *tile);
    };
    const auto ringed = [](std::span<const DVec2> rings) {
        return std::ranges::count_if(rings, [](DVec2 slope) { return glm::length(slope) > 0.01; });
    };
    // Without rain, nothing's drawn.
    const std::vector<DVec2> dry = drawn_with(0.0);
    CHECK(std::ranges::all_of(dry, [](DVec2 slope) { return slope == DVec2{0.0}; }));
    const double light = hog::render::rain_ring_chance(2.0, 0.0);
    const std::vector<DVec2> few = drawn_with(light);
    const std::vector<DVec2> many = drawn_with(1.0);
    CAPTURE(ringed(few), ringed(many));
    CHECK(ringed(few) > 0);
    CHECK(ringed(many) > 2 * ringed(few));
    // Each layer rings its own way.
    const std::span<const DVec2> first = std::span{many}.first(rain_texels);
    const std::span<const DVec2> second = std::span{many}.subspan(rain_texels);
    CHECK_FALSE(std::ranges::equal(first, second));
    const glm::dvec2 rain{1.0, 12.3};
    const auto drawn_at = [&](int x, int y, int layer) {
        return many.at((static_cast<std::size_t>(layer) * rain_texels) +
                       (static_cast<std::size_t>(y) * rain_tile_size) +
                       static_cast<std::size_t>(x));
    };
    double worst = 0.0;
    double worst_edge = 0.0;
    for (int layer = 0; std::cmp_less(layer, rain_tile_layers); ++layer) {
        for (int y = 0; std::cmp_less(y, rain_tile_size); y += 5) {
            for (int x = 0; std::cmp_less(x, rain_tile_size); x += 7) {
                worst = std::max(worst, glm::length(drawn_at(x, y, layer) -
                                                    rings_at(rain_texel_at(x, y), layer, rain)));
            }
            // What's drawn at the tile's first column and row carries on from its last.
            const DVec2 past{rain_tile_cells, 0.0};
            worst_edge = std::max(worst_edge,
                                  glm::length(drawn_at(0, y, layer) -
                                              rings_at(rain_texel_at(0, y) + past, layer, rain)));
            worst_edge = std::max(
                worst_edge,
                glm::length(drawn_at(y, 0, layer) -
                            rings_at(rain_texel_at(y, 0) + DVec2{past.y, past.x}, layer, rain)));
        }
    }
    CHECK(worst < 1e-3);
    CHECK(worst_edge < 1e-3);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("ripples nothing stirs calm, then are left still and no longer stepped until stirred",
          "[render][gpu]") {
    const auto device = test_device();
    auto map = hog::render::RippleMap::create(*device);
    REQUIRE(map);
    auto calm = hog::render::CalmSea::create(*device);
    REQUIRE(calm);
    const auto stepped = [&](std::span<const RippleBody> bodies = {}) {
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(map->step(commands, {.shift = {0, 0}, .seconds = 1.0, .kept = true},
                              calm->bindings(), 0, bodies));
        }));
        return kept_texels(*device, *map);
    };
    // Away from the edge, where they're soaked up.
    const auto inner_height = [](std::span<const DVec2> ripples) {
        double sum = 0.0;
        for (int y = 128; y < 384; ++y) {
            for (int x = 128; x < 384; ++x) {
                sum += ripples[index_of(x, y)].x * ripples[index_of(x, y)].x;
            }
        }
        return sum;
    };
    const double turn = 2.0 * std::numbers::pi / (ripple_size * ripple_spacing);
    const std::vector<DVec2> laid = train({64.0 * turn, 0.0}, 0.02, 0.0);
    lay_kept(*device, *map, laid);
    map->stir();
    std::vector<DVec2> ran;
    double before_calming = 0.0;
    const int calming_from = static_cast<int>(ripple_settling - ripple_calming);
    for (int second = 1; second < static_cast<int>(ripple_settling); ++second) {
        ran = stepped();
        if (second == calming_from) {
            before_calming = inner_height(ran);
        }
    }
    CHECK(before_calming > inner_height(laid) * 0.5);
    // Calmed before they're left still, so they're never seen to vanish.
    CHECK(inner_height(ran) < before_calming * 0.01);
    ran = stepped();
    CHECK(std::ranges::all_of(ran, [](DVec2 ripple) { return ripple == DVec2{0.0}; }));
    // Left still, ripples aren't stepped, even those laid on afresh.
    lay_kept(*device, *map, laid);
    const std::vector<glm::vec2> as_laid{laid.begin(), laid.end()};
    const std::vector<DVec2> kept{as_laid.begin(), as_laid.end()};
    CHECK(stepped() == kept);
    // Until a body stirs them.
    const RippleBody wading{.feet = {256.0F, 256.0F}, .above = -0.5F};
    CHECK(stepped(std::span{&wading, 1}) != kept);
    CHECK(device->validation_errors() == 0);
}
