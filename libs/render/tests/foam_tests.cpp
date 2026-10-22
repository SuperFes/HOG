#include "test_device.hpp"

#include <hog/core/math.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/foam.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/readback.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/waves/foam.hpp>
#include <hog/waves/waves.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec2;
using hog::DVec3;
using hog::render::foam_size;
using hog::render::foam_spacing;
using hog::render::FoamGrid;
using hog::render::FoamStep;
using hog::render::testing::test_device;

namespace {

constexpr double radius = 115'000.0;
constexpr double height = 2.0;
// Where the eye starts, and east and north along the ground there.
struct Start {
    DVec3 up = glm::normalize(DVec3{0.2, -0.1, 1.0});
    DVec3 east = glm::normalize(glm::cross(DVec3{0.0, 0.0, 1.0}, up));
    DVec3 north = glm::cross(up, east);
};

// Over sea level `along` metres east and north of the start, along the ground.
DVec3 up_at(DVec2 along) {
    const Start at;
    return glm::normalize((at.up * radius) + (at.east * along.x) + (at.north * along.y));
}

// Of `point`, on sea level, the texel it lies on from an eye over `up`.
DVec2 texel_of(const FoamGrid& grid, DVec3 up, DVec3 point) {
    return hog::render::foam_texel(grid.placing(up, radius, height),
                                   (point * radius) - (up * (radius + height)));
}

} // namespace

TEST_CASE("the foam grid stays centred under the eye a whole texel at a time", "[render]") {
    const Start at;
    const DVec3 start = at.up;
    FoamGrid grid;
    const FoamStep laid = grid.follow(start, radius, DVec3{0.0}, 0.0);
    CHECK(!laid.kept);
    const DVec2 middle{foam_size / 2.0};
    CHECK_THAT(glm::distance(texel_of(grid, start, start), middle), WithinAbs(0.0, 0.5));
    // A little way on, it doesn't step; further on, it steps by as many texels as it's gone.
    const DVec3 near = up_at({0.3, 0.0});
    const FoamStep stayed = grid.follow(near, radius, DVec3{0.0}, 0.1);
    CHECK(stayed.kept);
    CHECK(stayed.shift == glm::ivec2{0});
    const DVec3 on = up_at({10.4 * foam_spacing, -3.2 * foam_spacing});
    const FoamStep stepped = grid.follow(on, radius, DVec3{0.0}, 0.1);
    CHECK(stepped.kept);
    CHECK(stepped.shift == glm::ivec2{10, -3});
    CHECK(glm::distance(texel_of(grid, on, on), middle) <= (0.5 * std::sqrt(2.0)) + 1e-3);
    // What's on the sea stays on the same texel as the grid steps under it, but for the step.
    const DVec3 mark = up_at({40.0, 25.0});
    const DVec2 before = texel_of(grid, on, mark);
    const DVec3 further = up_at({30.0, 20.0});
    const FoamStep again = grid.follow(further, radius, DVec3{0.0}, 0.1);
    const DVec2 after = texel_of(grid, further, mark);
    CHECK_THAT(glm::distance(after + DVec2{again.shift}, before), WithinAbs(0.0, 1e-3));
}

TEST_CASE("the foam grid drifts with the water at the surface", "[render]") {
    const Start at;
    const DVec3 start = at.up;
    FoamGrid grid;
    (void)grid.follow(start, radius, DVec3{0.0}, 0.0);
    const DVec3 drift = (at.east * 0.6) - (at.north * 0.2);
    // A fleck of foam on the water drifts with it, and stays on the same texel but for the grid's
    // steps.
    const DVec2 laid = texel_of(grid, start, up_at({5.0, 5.0}));
    glm::ivec2 shifted{0};
    for (int second = 1; second <= 10; ++second) {
        const FoamStep step = grid.follow(start, radius, drift, 1.0);
        REQUIRE(step.kept);
        shifted += step.shift;
        const DVec2 drifted = DVec2{5.0, 5.0} + (DVec2{0.6, -0.2} * static_cast<double>(second));
        const DVec2 now = texel_of(grid, start, up_at(drifted)) + DVec2{shifted};
        CHECK_THAT(glm::distance(now, laid), WithinAbs(0.0, 1e-3));
    }
    // The eye stayed put, so the grid stepped back against the drift.
    CHECK(shifted == glm::ivec2{-6, 2});
}

TEST_CASE("the foam grid keeps nothing over a long pause, a step back in time or a long way",
          "[render]") {
    const Start at;
    const DVec3 start = at.up;
    FoamGrid grid;
    (void)grid.follow(start, radius, DVec3{0.0}, 0.0);
    CHECK(grid.follow(start, radius, DVec3{0.0}, 0.5).kept);
    CHECK(!grid.follow(start, radius, DVec3{0.0}, hog::render::foam_longest_step + 0.5).kept);
    CHECK(!grid.follow(start, radius, DVec3{0.0}, -0.5).kept);
    CHECK(grid.follow(start, radius, DVec3{0.0}, 0.5).kept);
    // Further than the grid is wide.
    CHECK(!grid.follow(up_at({foam_size * foam_spacing, 0.0}), radius, DVec3{0.0}, 0.5).kept);
    // Far from where the plane touched sea level, it's laid again, under the eye.
    const DVec3 far = up_at({hog::render::foam_plane_reach * 1.5, 0.0});
    (void)grid.follow(up_at({hog::render::foam_plane_reach * 0.8, 0.0}), radius, DVec3{0.0}, 0.5);
    CHECK(!grid.follow(far, radius, DVec3{0.0}, 0.5).kept);
    CHECK_THAT(glm::distance(texel_of(grid, far, far), DVec2{foam_size / 2.0}),
               WithinAbs(0.0, 0.5));
    CHECK_THAT(glm::length(grid.placing(far, radius, height).anchor), WithinAbs(height, 1e-6));
}

TEST_CASE("foam left fades and stays on the sea as the grid steps", "[render][gpu]") {
    const auto device = test_device();
    auto map = hog::render::FoamMap::create(*device);
    REQUIRE(map);
    // Each texel as much as its place.
    std::vector<float> laid(std::size_t{foam_size} * foam_size);
    for (std::size_t i = 0; i < laid.size(); ++i) {
        const std::size_t row = i / foam_size;
        laid[i] = static_cast<float>(i % foam_size) + (0.001F * static_cast<float>(row));
    }
    auto staging = hog::render::make_filled_buffer(*device, std::as_bytes(std::span{laid}),
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    REQUIRE(staging);
    REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
        VkImage kept = map->kept().handle();
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
            .imageExtent = {.width = foam_size, .height = foam_size, .depth = 1}};
        vkCmdCopyBufferToImage(commands, staging->handle(), kept,
                               hog::render::image_use::copy_target.layout, 1, &copy);
        hog::render::transition(commands, kept, VK_IMAGE_ASPECT_COLOR_BIT,
                                hog::render::image_use::copy_target,
                                hog::render::image_use::sampled);
    }));
    auto calm = hog::render::CalmSea::create(*device);
    REQUIRE(calm);
    const auto stepped = [&](const FoamStep& step) {
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            map->step(commands, step, calm->bindings(), DVec3{0.0});
        }));
        auto pixels =
            hog::render::read_pixels(*device, map->kept(), hog::render::image_use::sampled);
        REQUIRE(pixels);
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            hog::render::transition(commands, map->kept().handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                    hog::render::image_use::copy_source,
                                    hog::render::image_use::sampled);
        }));
        std::vector<float> foam(laid.size());
        REQUIRE(pixels->size() == std::span{foam}.size_bytes());
        std::memcpy(foam.data(), pixels->data(), pixels->size());
        return foam;
    };
    const glm::ivec2 shift{3, -2};
    const std::vector<float> foam =
        stepped({.shift = shift, .seconds = hog::render::foam_lasting, .kept = true});
    const auto at = [](const std::vector<float>& texels, int x, int y) {
        return static_cast<double>(
            texels.at((static_cast<std::size_t>(y) * foam_size) + static_cast<std::size_t>(x)));
    };
    for (const glm::ivec2 texel :
         {glm::ivec2{0, 2}, glm::ivec2{100, 300}, glm::ivec2{foam_size - 4, foam_size - 1}}) {
        const glm::ivec2 from = texel + shift;
        CHECK_THAT(at(foam, texel.x, texel.y),
                   WithinRel(at(laid, from.x, from.y) / std::exp(1.0), 1e-3));
    }
    // What was beyond the grid's edge before it stepped isn't known.
    CHECK(at(foam, foam_size - 1, 10) == 0.0);
    CHECK(at(foam, 10, 0) == 0.0);
    CHECK(at(foam, 10, 1) == 0.0);
    // Nor anything at all where nothing's kept.
    const std::vector<float> none = stepped({.shift = {0, 0}, .seconds = 0.1, .kept = false});
    CHECK(at(none, 100, 300) == 0.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("foam left covers as much of the sea as the wind leaves white, in patches",
          "[render][gpu]") {
    const auto device = test_device();
    auto ocean =
        hog::render::Ocean::create(*device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(ocean);
    const Start at;
    const std::vector<hog::waves::Train> trains = hog::waves::planet_trains(5);
    const hog::WorldPosition eye{at.up * (radius + 8.0)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, 0.0);
    // Late in 2026, so phases are taken far from the epoch, as they will be; each sea an hour
    // apart, so none is kept from the one before.
    hog::GameTime time{std::chrono::seconds{1'792'000'000}};
    for (const double wind : {10.0, 16.0}) {
        time += std::chrono::hours{1};
        const DVec3 blowing = at.east * wind;
        const hog::waves::SeaState sea{
            .systems = {hog::waves::grown_sea(blowing)}, .current = {}, .wind = blowing};
        hog::render::OceanView view;
        view.bend = bend;
        view.waves = hog::render::drawn_waves_at(trains, hog::waves::waves_at(trains, bend.up, sea),
                                                 sea, eye, bend, time, DVec3{0.0});
        hog::Result<> stepped;
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            stepped = ocean->step_foam(commands, view, 0, time, hog::waves::foam_drift(sea));
        }));
        REQUIRE(stepped);
        auto pixels = hog::render::read_pixels(*device, ocean->foam().kept(),
                                               hog::render::image_use::sampled);
        REQUIRE(pixels);
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            hog::render::transition(commands, ocean->foam().kept().handle(),
                                    VK_IMAGE_ASPECT_COLOR_BIT, hog::render::image_use::copy_source,
                                    hog::render::image_use::sampled);
        }));
        std::vector<float> foam(std::size_t{foam_size} * foam_size);
        REQUIRE(pixels->size() == std::span{foam}.size_bytes());
        std::memcpy(foam.data(), pixels->data(), pixels->size());
        double sum = 0.0;
        std::size_t holding = 0;
        for (const float texel : foam) {
            sum += static_cast<double>(texel);
            holding += texel > 0.05F ? 1U : 0U;
        }
        const double mean = sum / static_cast<double>(foam.size());
        const double most = static_cast<double>(*std::ranges::max_element(foam));
        const double share = static_cast<double>(holding) / static_cast<double>(foam.size());
        const hog::waves::Whitecaps white = hog::waves::whitecaps(sea);
        INFO(wind << " m/s: " << mean << " left of " << white.left << ", most " << most << " on "
                  << share << " of the sea");
        CHECK_THAT(mean, WithinRel(white.left, 0.25));
        // Heaped behind where crests broke, and none on most of the sea.
        CHECK(most > 0.5);
        CHECK(share < 0.5);
    }
    CHECK(device->validation_errors() == 0);
}
