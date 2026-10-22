#include "test_device.hpp"

#include <hog/core/math.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/foam.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/readback.hpp>
#include <hog/shaders/foam_probe.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/waves/foam.hpp>
#include <hog/waves/waves.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <random>
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

// Each texel the map keeps: the foam left there, and how much of what would have left it is known.
std::vector<DVec2> kept_texels(hog::render::Device& device, const hog::render::FoamMap& map) {
    auto pixels = hog::render::read_pixels(device, map.kept(), hog::render::image_use::sampled);
    REQUIRE(pixels);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        hog::render::transition(commands, map.kept().handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                hog::render::image_use::copy_source,
                                hog::render::image_use::sampled);
    }));
    std::vector<glm::vec4> kept(std::size_t{foam_size} * foam_size);
    REQUIRE(pixels->size() == std::span{kept}.size_bytes());
    std::memcpy(kept.data(), pixels->data(), pixels->size());
    std::vector<DVec2> texels;
    texels.reserve(kept.size());
    for (const glm::vec4& texel : kept) {
        texels.emplace_back(texel.x, texel.y);
    }
    return texels;
}

// Fills each texel `map` keeps with as much foam left, as known.
void fill_kept(hog::render::Device& device, const hog::render::FoamMap& map, DVec2 kept) {
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        VkImage image = map.kept().handle();
        hog::render::transition(commands, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                hog::render::image_use::sampled,
                                hog::render::image_use::copy_target);
        const VkClearColorValue colour{
            .float32 = {static_cast<float>(kept.x), static_cast<float>(kept.y), 0.0F, 0.0F}};
        const VkImageSubresourceRange whole{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                            .baseMipLevel = 0,
                                            .levelCount = 1,
                                            .baseArrayLayer = 0,
                                            .layerCount = 1};
        vkCmdClearColorImage(commands, image, hog::render::image_use::copy_target.layout, &colour,
                             1, &whole);
        hog::render::transition(commands, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                hog::render::image_use::copy_target,
                                hog::render::image_use::sampled);
    }));
}

// Of each point of sea level from the eye `rest`, over deep water as `drawn` has it, seen over
// `footprint` metres: x how much is white with foam left, with what `map` keeps; y as much as is
// likely without it; and zw what's kept there.
std::vector<glm::dvec4> probe_foam(hog::render::Device& device,
                                   const hog::render::DrawnWaves& drawn,
                                   const hog::render::FoamMap& map, std::span<const DVec3> rest,
                                   double footprint) {
    std::vector<glm::vec4> asked;
    asked.reserve(rest.size());
    for (const DVec3& point : rest) {
        asked.emplace_back(hog::Vec3{point}, static_cast<float>(footprint));
    }
    constexpr auto binding = [](std::uint32_t at, VkDescriptorType type) {
        return VkDescriptorSetLayoutBinding{.binding = at,
                                            .descriptorType = type,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = nullptr};
    };
    const std::array bindings{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                              binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                              binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                              binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
    auto probe = hog::render::make_compute_pipeline(
        device, {.spirv = hog::shaders::foam_probe, .descriptors = bindings});
    REQUIRE(probe);
    auto waves = hog::render::make_filled_buffer(device, std::as_bytes(std::span{&drawn, 1}),
                                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    REQUIRE(waves);
    auto questions = hog::render::make_filled_buffer(device, std::as_bytes(std::span{asked}),
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(questions);
    std::vector<glm::vec4> found(rest.size());
    auto answers =
        hog::render::make_buffer(device, std::span{found}.size_bytes(),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(answers);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe->handle());
        hog::render::push_uniform_buffer(commands, *probe, 0, waves->handle());
        hog::render::push_sampled_image(commands, *probe, 1, map.kept().view(), map.sampler());
        hog::render::push_storage_buffer(commands, *probe, 2, questions->handle());
        hog::render::push_storage_buffer(commands, *probe, 3, answers->handle());
        vkCmdDispatch(commands, static_cast<std::uint32_t>((rest.size() + 63) / 64), 1, 1);
        const VkMemoryBarrier2 to_host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .pNext = nullptr,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                       .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .pNext = nullptr,
                                          .dependencyFlags = 0,
                                          .memoryBarrierCount = 1,
                                          .pMemoryBarriers = &to_host,
                                          .bufferMemoryBarrierCount = 0,
                                          .pBufferMemoryBarriers = nullptr,
                                          .imageMemoryBarrierCount = 0,
                                          .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(commands, &dependency);
    }));
    REQUIRE(answers->invalidate());
    std::memcpy(found.data(), answers->mapped().data(), std::span{found}.size_bytes());
    return {found.begin(), found.end()};
}

DVec2 texel_at(const std::vector<DVec2>& texels, int x, int y) {
    return texels.at((static_cast<std::size_t>(y) * foam_size) + static_cast<std::size_t>(x));
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

TEST_CASE("the foam grid keeps nothing over a long pause, a step back in time, a long way or "
          "when it's to forget it",
          "[render]") {
    const Start at;
    const DVec3 start = at.up;
    FoamGrid grid;
    (void)grid.follow(start, radius, DVec3{0.0}, 0.0);
    CHECK(grid.follow(start, radius, DVec3{0.0}, 0.5).kept);
    CHECK(!grid.follow(start, radius, DVec3{0.0}, hog::render::foam_longest_step + 0.5).kept);
    CHECK(!grid.follow(start, radius, DVec3{0.0}, -0.5).kept);
    CHECK(grid.follow(start, radius, DVec3{0.0}, 0.5).kept);
    grid.forget();
    CHECK(!grid.follow(start, radius, DVec3{0.0}, 0.5).kept);
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
    // Each texel as much as its place, and as known.
    std::vector<glm::vec4> laid;
    laid.reserve(std::size_t{foam_size} * foam_size);
    for (std::uint32_t y = 0; y < foam_size; ++y) {
        for (std::uint32_t x = 0; x < foam_size; ++x) {
            laid.emplace_back(static_cast<float>(x) / static_cast<float>(foam_size),
                              static_cast<float>(y) / static_cast<float>(foam_size), 0.0F, 0.0F);
        }
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
        return kept_texels(*device, *map);
    };
    const glm::ivec2 shift{3, -2};
    const std::vector<DVec2> foam =
        stepped({.shift = shift, .seconds = hog::render::foam_lasting, .kept = true});
    // Over the step, as much again comes to be known as was unknown, less what's faded.
    const double fading = 1.0 / std::numbers::e;
    for (const glm::ivec2 texel :
         {glm::ivec2{0, 2}, glm::ivec2{100, 300}, glm::ivec2{foam_size - 4, foam_size - 1}}) {
        const DVec2 from = DVec2{texel + shift} / static_cast<double>(foam_size);
        const DVec2 now = texel_at(foam, texel.x, texel.y);
        CHECK_THAT(now.x, WithinRel(from.x * fading, 1e-4));
        CHECK_THAT(now.y, WithinRel((from.y * fading) + 1.0 - fading, 1e-4));
    }
    // What was beyond the grid's edge before it stepped isn't known.
    for (const glm::ivec2 texel :
         {glm::ivec2{foam_size - 1, 10}, glm::ivec2{10, 0}, glm::ivec2{10, 1}}) {
        const DVec2 now = texel_at(foam, texel.x, texel.y);
        CHECK(now.x == 0.0);
        CHECK_THAT(now.y, WithinRel(1.0 - fading, 1e-4));
    }
    // Nor anything at all where nothing's kept, but what's caught up on.
    const std::vector<DVec2> none = stepped({.shift = {0, 0}, .seconds = 0.1, .kept = false});
    const double caught_up = (hog::render::foam_catch_up + 1) * hog::render::foam_catch_up_step;
    CHECK(texel_at(none, 100, 300).x == 0.0);
    CHECK_THAT(texel_at(none, 100, 300).y,
               WithinRel(1.0 - std::exp(-caught_up / hog::render::foam_lasting), 1e-4));
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
        const std::vector<DVec2> foam = kept_texels(*device, ocean->foam());
        double sum = 0.0;
        double most = 0.0;
        std::size_t holding = 0;
        for (const DVec2 texel : foam) {
            sum += texel.x;
            most = std::max(most, texel.x);
            holding += texel.x > 0.05 ? 1U : 0U;
        }
        const double mean = sum / static_cast<double>(foam.size());
        const double share = static_cast<double>(holding) / static_cast<double>(foam.size());
        const hog::waves::Whitecaps white = hog::waves::whitecaps(sea);
        INFO(wind << " m/s: " << mean << " left of " << white.left << ", most " << most << " on "
                  << share << " of the sea");
        CHECK_THAT(mean, WithinRel(white.left, 0.25));
        // Caught up over long enough to know most of what left it, even in the surf.
        const double caught_up = (hog::render::foam_catch_up + 1) * hog::render::foam_catch_up_step;
        const double least_known = std::ranges::min(foam, {}, &DVec2::y).y;
        CHECK(least_known > 1.0 - std::exp(-caught_up / hog::render::foam_lasting_surf) - 1e-4);
        // Heaped behind where crests broke, and none on most of the sea.
        CHECK(most > 0.5);
        CHECK(share < 0.5);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("foam kept round the eye is drawn as much as is kept, what's not known as is likely",
          "[render][gpu]") {
    const auto device = test_device();
    auto map = hog::render::FoamMap::create(*device);
    REQUIRE(map);
    const Start at;
    const std::vector<hog::waves::Train> trains = hog::waves::planet_trains(5);
    const hog::WorldPosition eye{at.up * (radius + height)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, 0.0);
    const hog::GameTime time{std::chrono::seconds{1'792'000'000}};
    const auto drawn_over = [&](double wind) {
        const DVec3 blowing = at.east * wind;
        const hog::waves::SeaState sea{
            .systems = {hog::waves::grown_sea(blowing)}, .current = {}, .wind = blowing};
        hog::render::DrawnWaves drawn = hog::render::drawn_waves_at(
            trains, hog::waves::waves_at(trains, bend.up, sea), sea, eye, bend, time, DVec3{0.0});
        FoamGrid grid;
        static_cast<void>(grid.follow(at.up, radius, DVec3{0.0}, -1.0));
        const hog::render::FoamPlacing placing = grid.placing(at.up, radius, height);
        drawn.foam_anchor = {hog::Vec3{placing.anchor}, static_cast<float>(1.0 / foam_spacing)};
        drawn.foam_east = {hog::Vec3{placing.east}, static_cast<float>(placing.corner.x)};
        drawn.foam_north = {hog::Vec3{placing.north}, static_cast<float>(placing.corner.y)};
        return drawn;
    };
    const auto rest_at = [&](DVec2 along) {
        return (up_at(along) * radius) - (at.up * (radius + height));
    };
    std::mt19937_64 random{5};
    std::uniform_real_distribution<double> spot{-150.0, 150.0};
    std::vector<DVec3> near;
    near.reserve(4'000);
    for (int i = 0; i < 4'000; ++i) {
        near.push_back(rest_at(DVec2{spot(random), spot(random)}));
    }
    const std::array far{rest_at(DVec2{400.0, 0.0}), rest_at(DVec2{0.0, -400.0})};

    SECTION("over a calm sea, kept foam lies in lace, as much of it as is kept") {
        const hog::render::DrawnWaves calm = drawn_over(2.0);
        REQUIRE(calm.foam.z < 1e-4F);
        fill_kept(*device, *map, DVec2{0.4, 1.0});
        double sum = 0.0;
        int clear = 0;
        int white = 0;
        for (const glm::dvec4& probed : probe_foam(*device, calm, *map, near, 1e-2)) {
            CHECK_THAT(probed.z, WithinAbs(0.4, 1e-3));
            CHECK(probed.y < 0.05);
            sum += probed.x;
            clear += probed.x < 0.1 ? 1 : 0;
            white += probed.x > 0.7 ? 1 : 0;
        }
        const auto count = static_cast<double>(near.size());
        INFO(sum / count << " left, " << clear << " clear, " << white << " white");
        CHECK_THAT(sum / count, WithinRel(0.4, 0.1));
        CHECK(clear > count / 5.0);
        CHECK(white > count / 10.0);
        // From afar, each footprint is that much white.
        for (const glm::dvec4& probed : probe_foam(*device, calm, *map, near, 1e5)) {
            CHECK_THAT(probed.x, WithinAbs(0.4, 0.01));
        }
        // It fades out to the grid's edges, and there's none beyond.
        const std::array edge{rest_at(DVec2{(foam_size / 2.0) - 32.0, 0.0})};
        const glm::dvec4 fading = probe_foam(*device, calm, *map, edge, 1e5).front();
        CHECK_THAT(fading.z, WithinAbs(0.2, 0.01));
        CHECK_THAT(fading.w, WithinAbs(0.5, 0.01));
        CHECK_THAT(fading.x, WithinAbs(0.2, 0.01));
        for (const glm::dvec4& probed : probe_foam(*device, calm, *map, far, 1e-2)) {
            CHECK(probed.x == probed.y);
            CHECK(probed.z == 0.0);
        }
    }
    SECTION("over a windy sea, foam is left as likely where nothing's known") {
        const hog::render::DrawnWaves windy = drawn_over(16.0);
        fill_kept(*device, *map, DVec2{0.0, 0.0});
        double likely = 0.0;
        for (const glm::dvec4& probed : probe_foam(*device, windy, *map, near, 1e-2)) {
            CHECK_THAT(probed.x, WithinAbs(probed.y, 1e-5));
            likely += probed.y;
        }
        REQUIRE(likely > 0.02 * static_cast<double>(near.size()));
        // Where it's known there's none, there's none.
        fill_kept(*device, *map, DVec2{0.0, 1.0});
        for (const glm::dvec4& probed : probe_foam(*device, windy, *map, near, 1e-2)) {
            CHECK(probed.x == 0.0);
        }
        // Beyond the grid, all is as likely.
        for (const glm::dvec4& probed : probe_foam(*device, windy, *map, far, 1e-2)) {
            CHECK(probed.x == probed.y);
        }
    }
    CHECK(device->validation_errors() == 0);
}
