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
#include <hog/terrain/shore.hpp>
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
#include <optional>
#include <random>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec2;
using hog::DVec3;
using hog::render::DriftingGrid;
using hog::render::foam_size;
using hog::render::foam_spacing;
using hog::render::GridStep;
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

// Each texel the map keeps: the foam left there, how much of what would have left it is known, and
// the foam stranded on the ground there.
std::vector<DVec3> kept_texels(hog::render::Device& device, const hog::render::FoamMap& map) {
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
    std::vector<DVec3> texels;
    texels.reserve(kept.size());
    for (const glm::vec4& texel : kept) {
        texels.emplace_back(texel.x, texel.y, texel.z);
    }
    return texels;
}

// Fills each texel `map` keeps with as much foam left, as known, and as much stranded.
void fill_kept(hog::render::Device& device, const hog::render::FoamMap& map, DVec3 kept) {
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        VkImage image = map.kept().handle();
        hog::render::transition(commands, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                hog::render::image_use::sampled,
                                hog::render::image_use::copy_target);
        const VkClearColorValue colour{.float32 = {static_cast<float>(kept.x),
                                                   static_cast<float>(kept.y),
                                                   static_cast<float>(kept.z), 0.0F}};
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

// Lays `texels` on what `map` keeps, a row at a time.
void lay_kept(hog::render::Device& device, const hog::render::FoamMap& map,
              std::span<const glm::vec4> texels) {
    auto staging = hog::render::make_filled_buffer(device, std::as_bytes(texels),
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
            .imageExtent = {.width = foam_size, .height = foam_size, .depth = 1}};
        vkCmdCopyBufferToImage(commands, staging->handle(), kept,
                               hog::render::image_use::copy_target.layout, 1, &copy);
        hog::render::transition(commands, kept, VK_IMAGE_ASPECT_COLOR_BIT,
                                hog::render::image_use::copy_target,
                                hog::render::image_use::sampled);
    }));
}

// Of each point of sea level from the eye `rest`, over deep water as `drawn` has it, seen over
// `footprint` metres: x how much is white with foam left, with what `map` keeps; y as much as is
// likely without it; and zw what's kept there. Or as `entry` finds them in shaders/foam_probe.
std::vector<glm::dvec4> probe_foam(hog::render::Device& device,
                                   const hog::render::DrawnWaves& drawn,
                                   const hog::render::FoamMap& map, std::span<const DVec3> rest,
                                   double footprint, const char* entry = "compute_main") {
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
        device, {.spirv = hog::shaders::foam_probe, .entry = entry, .descriptors = bindings});
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

DVec3 texel_at(const std::vector<DVec3>& texels, int x, int y) {
    return texels.at((static_cast<std::size_t>(y) * foam_size) + static_cast<std::size_t>(x));
}

} // namespace

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
    lay_kept(*device, *map, laid);
    auto calm = hog::render::CalmSea::create(*device);
    REQUIRE(calm);
    const auto stepped = [&](const GridStep& step, bool catch_up = true) {
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            map->step(commands, step, calm->bindings(), DVec3{0.0}, catch_up);
        }));
        return kept_texels(*device, *map);
    };
    const glm::ivec2 shift{3, -2};
    const std::vector<DVec3> foam =
        stepped({.shift = shift, .seconds = hog::render::foam_lasting, .kept = true});
    // Over the step, as much again comes to be known as was unknown, less what's faded.
    const double fading = 1.0 / std::numbers::e;
    for (const glm::ivec2 texel :
         {glm::ivec2{0, 2}, glm::ivec2{100, 300}, glm::ivec2{foam_size - 4, foam_size - 1}}) {
        const DVec2 from = DVec2{texel + shift} / static_cast<double>(foam_size);
        const DVec3 now = texel_at(foam, texel.x, texel.y);
        CHECK_THAT(now.x, WithinRel(from.x * fading, 1e-4));
        CHECK_THAT(now.y, WithinRel((from.y * fading) + 1.0 - fading, 1e-4));
    }
    // What was beyond the grid's edge before it stepped isn't known.
    for (const glm::ivec2 texel :
         {glm::ivec2{foam_size - 1, 10}, glm::ivec2{10, 0}, glm::ivec2{10, 1}}) {
        const DVec3 now = texel_at(foam, texel.x, texel.y);
        CHECK(now.x == 0.0);
        CHECK_THAT(now.y, WithinRel(1.0 - fading, 1e-4));
    }
    // Nor anything at all where nothing's kept, but what's caught up on.
    const std::vector<DVec3> none = stepped({.shift = {0, 0}, .seconds = 0.1, .kept = false});
    const double caught_up = (hog::render::foam_catch_up + 1) * hog::render::foam_catch_up_step;
    CHECK(texel_at(none, 100, 300).x == 0.0);
    CHECK_THAT(texel_at(none, 100, 300).y,
               WithinRel(1.0 - std::exp(-caught_up / hog::render::foam_lasting), 1e-4));
    // Or, not caught up on, nothing's known.
    const std::vector<DVec3> unknown =
        stepped({.shift = {0, 0}, .seconds = 0.1, .kept = false}, false);
    CHECK(std::ranges::all_of(unknown, [](DVec3 texel) { return texel == DVec3{0.0}; }));
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
            stepped = ocean->step_foam(commands, view, 0, time, hog::waves::foam_drift(sea), true);
        }));
        REQUIRE(stepped);
        const std::vector<DVec3> foam = kept_texels(*device, ocean->foam());
        double sum = 0.0;
        double most = 0.0;
        std::size_t holding = 0;
        for (const DVec3 texel : foam) {
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
        const double least_known = std::ranges::min(foam, {}, &DVec3::y).y;
        CHECK(least_known > 1.0 - std::exp(-caught_up / hog::render::foam_lasting_surf) - 1e-4);
        // Heaped behind where crests broke, and none on most of the sea.
        CHECK(most > 0.5);
        CHECK(share < 0.5);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("foam the water draws back from is stranded on the ground, and stays with it till the "
          "water returns",
          "[render][gpu]") {
    const auto device = test_device();
    auto ocean =
        hog::render::Ocean::create(*device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(ocean);
    const Start at;
    hog::render::OceanView view;
    view.bend = hog::terrain::bend_from(hog::WorldPosition{at.up * (radius + height)}, radius, 0.0);
    // Level ground all round, `depth` metres under the sea's mean.
    const auto shore_of = [&](float depth) {
        const std::size_t texels = hog::terrain::shore_spacing.size() * hog::terrain::shore_size *
                                   hog::terrain::shore_size;
        return hog::terrain::ShoreDepths{
            .frame = hog::terrain::shore_frame(view.bend.up, radius),
            .depths = std::vector<float>(texels, depth),
            .still = std::vector<float>(texels, hog::terrain::shore_highest),
            .levels = std::vector<float>(texels, 0.0F),
            .fetches = std::vector<float>(texels * hog::terrain::fetch_headings, 0.0F),
            .offshore = {},
            .plumes = {}};
    };
    hog::GameTime time{std::chrono::seconds{1'792'000'000}};
    const auto stepped = [&](std::optional<float> depth, DVec3 drift) {
        hog::Result<> foamed;
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            if (depth) {
                REQUIRE(ocean->update_shore(commands, 0, shore_of(*depth)));
            }
            foamed = ocean->step_foam(commands, view, 0, time, drift, true);
        }));
        REQUIRE(foamed);
        return kept_texels(*device, ocean->foam());
    };
    const double stranded_fading = std::exp(-1.0 / hog::render::foam_lasting_stranded);
    constexpr std::array texels{glm::ivec2{10, 20}, glm::ivec2{256, 256}, glm::ivec2{500, 400}};

    SECTION("on dry ground, some of what's on the water is stranded; and taken back up as it "
            "returns") {
        const double stranded = 0.2 + (hog::render::foam_stranding * 0.5);
        static_cast<void>(stepped(-0.5F, DVec3{0.0}));
        fill_kept(*device, ocean->foam(), DVec3{0.5, 1.0, 0.2});
        time += std::chrono::seconds{1};
        const std::vector<DVec3> dry = stepped(std::nullopt, DVec3{0.0});
        for (const glm::ivec2 texel : texels) {
            const DVec3 now = texel_at(dry, texel.x, texel.y);
            CHECK(now.x == 0.0);
            CHECK_THAT(now.y, WithinAbs(1.0, 1e-6));
            CHECK_THAT(now.z, WithinRel(stranded * stranded_fading, 1e-4));
        }
        time += std::chrono::seconds{1};
        const std::vector<DVec3> wet = stepped(2.0F, DVec3{0.0});
        for (const glm::ivec2 texel : texels) {
            const DVec3 now = texel_at(wet, texel.x, texel.y);
            CHECK_THAT(now.x, WithinRel(stranded * stranded_fading *
                                            std::exp(-1.0 / hog::render::foam_lasting),
                                        1e-4));
            CHECK(now.z == 0.0);
        }
    }
    SECTION("what's stranded stays where it lies on the ground as the grid drifts on") {
        DriftingGrid grid{foam_size, foam_spacing};
        static_cast<void>(grid.follow(view.bend.up, radius, DVec3{0.0}, -1.0));
        const hog::render::GridPlacing laid = grid.placing(view.bend.up, radius, height);
        // Over a whole texel and part of one, so the grid steps as well.
        const DVec3 drift = (laid.east * 1.3) + (laid.north * -0.4);
        static_cast<void>(stepped(-0.5F, drift));
        // As much stranded on each texel as how far along its row it lies.
        std::vector<glm::vec4> along;
        along.reserve(std::size_t{foam_size} * foam_size);
        for (std::uint32_t y = 0; y < foam_size; ++y) {
            for (std::uint32_t x = 0; x < foam_size; ++x) {
                along.emplace_back(0.0F, 1.0F,
                                   static_cast<float>(x) / static_cast<float>(foam_size), 0.0F);
            }
        }
        lay_kept(*device, ocean->foam(), along);
        time += std::chrono::seconds{1};
        const std::vector<DVec3> drifted = stepped(std::nullopt, drift);
        const GridStep step = grid.follow(view.bend.up, radius, drift, 1.0);
        REQUIRE(step.shift != glm::ivec2{0});
        const hog::render::GridPlacing placing = grid.placing(view.bend.up, radius, height);
        for (const glm::ivec2 texel : texels) {
            // Where that texel's middle lay on the grid before it drifted.
            const DVec2 before =
                DVec2{texel} + 0.5 + ((placing.corner - laid.corner) / foam_spacing);
            const DVec3 now = texel_at(drifted, texel.x, texel.y);
            CHECK_THAT(now.z, WithinAbs((before.x - 0.5) / foam_size * stranded_fading, 1e-5));
        }
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
    // With the grid laid a second before, the water drifting `drifting` m/s along it since.
    const auto drawn_over = [&](double wind, DVec2 drifting = DVec2{0.0}) {
        const DVec3 blowing = at.east * wind;
        const hog::waves::SeaState sea{
            .systems = {hog::waves::grown_sea(blowing)}, .current = {}, .wind = blowing};
        DriftingGrid grid{foam_size, foam_spacing};
        static_cast<void>(grid.follow(at.up, radius, DVec3{0.0}, -1.0));
        const hog::render::GridPlacing laid = grid.placing(at.up, radius, height);
        const DVec3 drift = (laid.east * drifting.x) + (laid.north * drifting.y);
        static_cast<void>(grid.follow(at.up, radius, drift, 1.0));
        const hog::render::GridPlacing placing = grid.placing(at.up, radius, height);
        hog::render::DrawnWaves drawn = hog::render::drawn_waves_at(
            trains, hog::waves::waves_at(trains, bend.up, sea), sea, eye, bend, time, drift);
        drawn.foam_anchor = {hog::Vec3{placing.anchor}, static_cast<float>(1.0 / foam_spacing)};
        drawn.foam_east = {hog::Vec3{placing.east}, static_cast<float>(placing.corner.x)};
        drawn.foam_north = {hog::Vec3{placing.north}, static_cast<float>(placing.corner.y)};
        drawn.foam_drifted = {static_cast<float>(placing.drifted.x),
                              static_cast<float>(placing.drifted.y), 0.0F, 0.0F};
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
        fill_kept(*device, *map, DVec3{0.4, 1.0, 0.0});
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
    SECTION("on the ground, stranded foam lies in lace, as much of it as is stranded, staying put "
            "as the water drifts on") {
        fill_kept(*device, *map, DVec3{0.4, 1.0, 0.4});
        const hog::render::DrawnWaves still = drawn_over(2.0);
        const hog::render::DrawnWaves drifted = drawn_over(2.0, DVec2{0.8, 0.3});
        const std::vector<glm::dvec4> before =
            probe_foam(*device, still, *map, near, 1e-2, "stranded_main");
        const std::vector<glm::dvec4> after =
            probe_foam(*device, drifted, *map, near, 1e-2, "stranded_main");
        double sum = 0.0;
        int clear = 0;
        int white = 0;
        for (std::size_t i = 0; i < near.size(); ++i) {
            CHECK_THAT(before.at(i).y, WithinAbs(0.4, 1e-3));
            CHECK_THAT(after.at(i).x, WithinAbs(before.at(i).x, 1e-3));
            sum += before.at(i).x;
            clear += before.at(i).x < 0.1 ? 1 : 0;
            white += before.at(i).x > 0.7 ? 1 : 0;
        }
        const auto count = static_cast<double>(near.size());
        INFO(sum / count << " stranded, " << clear << " clear, " << white << " white");
        CHECK_THAT(sum / count, WithinRel(0.4, 0.1));
        CHECK(clear > count / 5.0);
        CHECK(white > count / 10.0);
        // What's on the water drifts on with it.
        const std::vector<glm::dvec4> sea_before = probe_foam(*device, still, *map, near, 1e-2);
        const std::vector<glm::dvec4> sea_after = probe_foam(*device, drifted, *map, near, 1e-2);
        double moved = 0.0;
        for (std::size_t i = 0; i < near.size(); ++i) {
            moved += std::abs(sea_after.at(i).x - sea_before.at(i).x);
        }
        CHECK(moved > 0.1 * count);
        for (const glm::dvec4& probed :
             probe_foam(*device, still, *map, far, 1e-2, "stranded_main")) {
            CHECK(probed.x == 0.0);
        }
    }
    SECTION("over a windy sea, foam is left as likely where nothing's known") {
        const hog::render::DrawnWaves windy = drawn_over(16.0);
        fill_kept(*device, *map, DVec3{0.0});
        double likely = 0.0;
        for (const glm::dvec4& probed : probe_foam(*device, windy, *map, near, 1e-2)) {
            CHECK_THAT(probed.x, WithinAbs(probed.y, 1e-5));
            likely += probed.y;
        }
        REQUIRE(likely > 0.02 * static_cast<double>(near.size()));
        // Where it's known there's none, there's none.
        fill_kept(*device, *map, DVec3{0.0, 1.0, 0.0});
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
