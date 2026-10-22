#include "test_device.hpp"

#include <hog/core/math.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/ripples.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/shore.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec2;
using hog::DVec3;
using hog::render::GridStep;
using hog::render::ripple_size;
using hog::render::ripple_spacing;
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
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            map->step(commands, {.shift = {0, 0}, .seconds = seconds, .kept = true},
                      calm->bindings());
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
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            map->step(commands, {.shift = {0, 0}, .seconds = 1.0, .kept = true}, calm->bindings());
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
    const auto stepped = [&](const GridStep& step) {
        REQUIRE(device->submit_now(
            [&](VkCommandBuffer commands) { map->step(commands, step, calm->bindings()); }));
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
            .fetches = std::vector<float>(shore_texels * hog::terrain::fetch_headings, 0.0F)};
    };
    hog::GameTime time{std::chrono::seconds{1'792'000'000}};
    const std::vector<DVec2> laid = mound(0.05, 2.0);
    const auto stepped = [&](float depth) {
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            REQUIRE(ocean->update_shore(commands, 0, shore_of(depth)));
            REQUIRE(ocean->step_ripples(commands, view, 0, time, DVec3{0.0}));
        }));
        lay_kept(*device, ocean->ripples(), laid);
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
