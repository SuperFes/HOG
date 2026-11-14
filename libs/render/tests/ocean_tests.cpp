#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/shaders/shore_probe.hpp>
#include <hog/shaders/waves_probe.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/material.hpp>
#include <hog/terrain/shore.hpp>
#include <hog/terrain/water.hpp>
#include <hog/waves/foam.hpp>
#include <hog/waves/run_up.hpp>
#include <hog/waves/shoaling.hpp>
#include <hog/waves/tide.hpp>
#include <hog/waves/waves.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <numbers>
#include <optional>
#include <random>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::WorldPosition;
using hog::render::OceanView;
using hog::render::testing::test_device;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

constexpr VkExtent2D extent{.width = 64, .height = 64};
constexpr double radius = 115'000.0;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};
constexpr DVec3 north{0.0, 1.0, 0.0};

Pixel pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t first = 4 * ((std::size_t{y} * extent.width) + x);
    return {pixels.at(first), pixels.at(first + 1), pixels.at(first + 2), pixels.at(first + 3)};
}

constexpr double earth = 6'371'000.0;

// From walking height over open sea on an Earth-sized planet, looking level towards +X, the sky
// and the sea lit through the air; `place` has the sea first, to place still water on it, and
// `depths` what was drawn how near, as the depth image has it.
std::vector<std::uint8_t> through_air(hog::render::Device& device, double sun_elevation,
                                      const std::function<void(hog::render::Ocean&)>& place = {},
                                      std::vector<float>* depths = nullptr) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(sky);
    auto ocean =
        hog::render::Ocean::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, true);
    REQUIRE(ocean);
    if (place) {
        place(*ocean);
    }
    const WorldPosition eye{up * (earth + 2.0)};
    const double elevation = sun_elevation * std::numbers::pi / 180.0;
    const DVec3 sun = (east * std::cos(elevation)) + (up * std::sin(elevation));
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, earth, 0.0);
    const hog::render::Camera camera{.eye = eye, .forward = east, .up = up};
    const hog::render::SkyView seen{.camera = camera, .sun = sun, .bend = bend, .exposure = 10.0};
    const OceanView view{
        .camera = camera, .sun = sun, .bend = bend, .air = sky->bindings(0, *atmosphere)};

    auto target = hog::render::make_image(
        device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(target);
    auto depth =
        hog::render::make_image(device, {.format = VK_FORMAT_D32_SFLOAT,
                                         .extent = extent,
                                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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
        sky->draw(commands, extent, 0, *atmosphere, seen);
        CHECK(ocean->draw(commands, extent, view));
        vkCmdEndRendering(commands);
    }));
    if (depths != nullptr) {
        auto read = hog::render::read_pixels(device, *depth, hog::render::image_use::depth_target);
        REQUIRE(read);
        depths->resize(read->size() / sizeof(float));
        std::memcpy(depths->data(), read->data(), read->size());
    }
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

float depth_at(const std::vector<float>& depths, std::uint32_t x, std::uint32_t y) {
    return depths.at((std::size_t{y} * extent.width) + x);
}

int brightness(Pixel pixel) {
    return pixel[0] + pixel[1] + pixel[2];
}

} // namespace

TEST_CASE("through the air, the sea meets the sky at the horizon without a seam",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    // The sun behind, so neither its disc nor its glint is in view.
    const auto pixels = through_air(*device, -150.0);
    const auto daytime = through_air(*device, 160.0);
    const std::uint32_t middle = extent.width / 2;
    const std::uint32_t horizon = extent.height / 2;
    const Pixel above = pixel_at(daytime, middle, horizon - 1);
    const Pixel below = pixel_at(daytime, middle, horizon + 1);
    INFO("above " << int{above[0]} << ", " << int{above[1]} << ", " << int{above[2]} << "; below "
                  << int{below[0]} << ", " << int{below[1]} << ", " << int{below[2]});
    // Just below, it reflects most of the sky just above.
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CHECK(below.at(channel) <= above.at(channel));
        CHECK(below.at(channel) >= 0.8 * above.at(channel));
    }
    // The steeper the view down, the less the sea reflects.
    CHECK(brightness(pixel_at(daytime, middle, extent.height - 1)) < brightness(below));
    CHECK(below[2] > below[0]);
    // At night, it's dark.
    CHECK(brightness(pixel_at(pixels, middle, horizon + 1)) < 15);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("still water placed as a mesh is drawn where it stands, with the sea",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    std::vector<float> without_depths;
    const auto without = through_air(*device, 160.0, {}, &without_depths);
    // Two sheets of still water running out east: one a metre over the sea, under the eye, and
    // one five metres over the eye. The first is seen as the sea is, nearer; the second from
    // under, where the sky was, and a surface seen from its back mirrors what's behind it.
    const auto sheet = [](double height) {
        auto made = std::make_shared<hog::terrain::BlockMesh>();
        made->origin = WorldPosition{up * (earth + height)};
        for (const hog::Vec3 corner :
             {hog::Vec3{3.0F, -300.0F, 0.0F}, hog::Vec3{300.0F, -300.0F, 0.0F},
              hog::Vec3{300.0F, 300.0F, 0.0F}, hog::Vec3{3.0F, 300.0F, 0.0F}}) {
            made->vertices.push_back({.position = corner,
                                      .normal = hog::Vec3{0.0F, 0.0F, 1.0F},
                                      .tiles = {},
                                      .corner = 0});
        }
        made->indices = {0, 1, 2, 0, 2, 3};
        return made;
    };
    std::vector<float> with_depths;
    const auto with = through_air(
        *device, 160.0,
        [&](hog::render::Ocean& ocean) {
            ocean.place_still(7, sheet(1.0));
            ocean.place_still(8, sheet(7.0));
            ocean.place_still(9, sheet(7.0));
            ocean.remove_still(9);
            ocean.place_still(10, std::make_shared<hog::terrain::BlockMesh>());
            CHECK(ocean.still_placed() == 2);
        },
        &with_depths);
    const std::uint32_t middle = extent.width / 2;
    const std::uint32_t horizon = extent.height / 2;
    // Nearer is deeper, and the sky is as far as can be.
    CHECK(depth_at(without_depths, middle, horizon - 6) == 0.0F);
    CHECK(depth_at(with_depths, middle, horizon - 6) > 0.0F);
    CHECK(depth_at(with_depths, middle, horizon + 6) >
          depth_at(without_depths, middle, horizon + 6));
    CHECK(depth_at(without_depths, middle, horizon + 6) > 0.0F);
    // Flat, the still water mirrors more of the sky at a glancing view than the sea's waves do,
    // in the same hue.
    const Pixel sea = pixel_at(without, middle, horizon + 6);
    const Pixel still = pixel_at(with, middle, horizon + 6);
    INFO("sea " << int{sea[0]} << ", " << int{sea[1]} << ", " << int{sea[2]} << "; still "
                << int{still[0]} << ", " << int{still[1]} << ", " << int{still[2]});
    CHECK(brightness(still) >= brightness(sea));
    CHECK(still[2] > still[0]);
    const double brighter = static_cast<double>(brightness(still)) / brightness(sea);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        CHECK_THAT(static_cast<double>(still.at(channel)) / sea.at(channel),
                   WithinAbs(brighter, 0.1));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a lake's water stands at its lowest, risen as far as the year has brought it",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    // A sheet a metre over the sea, rising five metres at its highest, which the eye two metres
    // up sees from under once it's risen: at its peak at the March equinox, as the drawn waves'
    // year has it, and half a year from it.
    const auto drawn = [&](std::uint8_t peak) {
        auto made = std::make_shared<hog::terrain::BlockMesh>();
        made->origin = WorldPosition{up * (earth + 1.0)};
        for (const hog::Vec3 corner :
             {hog::Vec3{3.0F, -300.0F, 0.0F}, hog::Vec3{300.0F, -300.0F, 0.0F},
              hog::Vec3{300.0F, 300.0F, 0.0F}, hog::Vec3{3.0F, 300.0F, 0.0F}}) {
            made->vertices.push_back({.position = corner,
                                      .normal = hog::Vec3{0.0F, 0.0F, 1.0F},
                                      .tiles = {0, 50, peak},
                                      .corner = 0});
        }
        made->indices = {0, 1, 2, 0, 2, 3};
        std::vector<float> depths;
        static_cast<void>(through_air(
            *device, 160.0, [&](hog::render::Ocean& ocean) { ocean.place_still(1, made); },
            &depths));
        return depths;
    };
    const std::uint32_t middle = extent.width / 2;
    const std::uint32_t over_horizon = (extent.height / 2) - 6;
    CHECK(depth_at(drawn(0), middle, over_horizon) > 0.0F);
    CHECK(depth_at(drawn(128), middle, over_horizon) == 0.0F);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("through the air, the sun's glint on the sea reddens as it sets",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    const auto setting = through_air(*device, 2.0);
    const std::uint32_t middle = extent.width / 2;
    Pixel brightest{};
    for (std::uint32_t y = (extent.height / 2) + 1; y < extent.height; ++y) {
        if (brightness(pixel_at(setting, middle, y)) > brightness(brightest)) {
            brightest = pixel_at(setting, middle, y);
        }
    }
    const Pixel aside = pixel_at(setting, 2, (extent.height / 2) + 4);
    INFO("glint " << int{brightest[0]} << ", " << int{brightest[1]} << ", " << int{brightest[2]}
                  << "; aside " << int{aside[0]} << ", " << int{aside[1]} << ", " << int{aside[2]});
    CHECK(brightness(brightest) > 2 * brightness(aside));
    CHECK(brightest[0] > 2 * brightest[2]);
    CHECK(device->validation_errors() == 0);
}

namespace {

// Late in 2026, so phases are taken far from the epoch, as they will be.
constexpr hog::GameTime noon{std::chrono::seconds{1'792'000'000}};

// Where the drawn waves move points of sea level `rest` from the eye, seen over `footprint`
// metres each, and how the surface faces there; how rough what's left is; how much of it is white
// breaking and with foam left; and the foam's coarse and fine pattern there, and how much of the
// fine is seen, which is none where it couldn't make any of it white.
struct Probed {
    DVec3 moved;
    DVec3 normal;
    double roughness;
    double breaking;
    double left;
    double coarse;
    double fine;
    double seen;
    // The root mean square of how high the drawn waves rise, and the share white in the surf.
    double heave;
    double surf;
    // How high the water stands over the point, the floor under it taken as under all it carries.
    double height;
};

// Over floors as wave_surface takes them, one for each point, or deep water; and over still water
// with as much fetch, one for each point, or the sea.
std::vector<Probed> probe_waves(hog::render::Device& device, const hog::render::DrawnWaves& drawn,
                                std::span<const DVec3> rest, double footprint,
                                std::span<const hog::Vec3> floors = {},
                                std::span<const float> fetches = {},
                                std::span<const hog::Vec3> offshore = {}) {
    std::vector<std::array<float, 4>> asked;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const DVec3& point = rest[i];
        asked.push_back({static_cast<float>(point.x), static_cast<float>(point.y),
                         static_cast<float>(point.z), static_cast<float>(footprint)});
        const hog::Vec3 floor = floors.empty() ? hog::Vec3{1.0e6F, 0.0F, 0.0F} : floors[i];
        asked.push_back({floor.x, floor.y, floor.z, fetches.empty() ? 0.0F : fetches[i]});
        const hog::Vec3 out =
            offshore.empty() ? hog::Vec3{hog::terrain::farthest_offshore, 0.0F, 0.0F} : offshore[i];
        asked.push_back({0.0F, out.x, out.y, out.z});
    }
    constexpr auto binding = [](std::uint32_t at, VkDescriptorType type) {
        return VkDescriptorSetLayoutBinding{.binding = at,
                                            .descriptorType = type,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = nullptr};
    };
    const std::array bindings{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                              binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                              binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
    auto probe = hog::render::make_compute_pipeline(
        device, {.spirv = hog::shaders::waves_probe, .descriptors = bindings});
    REQUIRE(probe);
    auto waves = hog::render::make_filled_buffer(device, std::as_bytes(std::span{&drawn, 1}),
                                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    REQUIRE(waves);
    auto questions = hog::render::make_filled_buffer(device, std::as_bytes(std::span{asked}),
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(questions);
    std::vector<std::array<float, 4>> found(4 * rest.size());
    auto answers =
        hog::render::make_buffer(device, std::span{found}.size_bytes(),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(answers);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe->handle());
        hog::render::push_uniform_buffer(commands, *probe, 0, waves->handle());
        hog::render::push_storage_buffer(commands, *probe, 1, questions->handle());
        hog::render::push_storage_buffer(commands, *probe, 2, answers->handle());
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
    std::vector<Probed> probed;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const auto& moved = found.at(4 * i);
        const auto& normal = found.at((4 * i) + 1);
        const auto& foam = found.at((4 * i) + 2);
        const auto& surf = found.at((4 * i) + 3);
        probed.push_back({.moved = DVec3{hog::Vec3{moved[0], moved[1], moved[2]}},
                          .normal = DVec3{hog::Vec3{normal[0], normal[1], normal[2]}},
                          .roughness = static_cast<double>(moved[3]),
                          .breaking = static_cast<double>(normal[3]),
                          .left = static_cast<double>(foam[0]),
                          .coarse = static_cast<double>(foam[1]),
                          .fine = static_cast<double>(foam[2]),
                          .seen = static_cast<double>(foam[3]),
                          .heave = static_cast<double>(surf[0]),
                          .surf = static_cast<double>(surf[1]),
                          .height = static_cast<double>(surf[2])});
    }
    return probed;
}

// `height` metres over a sea raised by a wind of `wind` m/s, somewhere off the top of the planet.
struct WavySea {
    std::vector<hog::waves::Train> trains = hog::waves::planet_trains(5);
    WorldPosition eye;
    hog::terrain::Bend bend;
    hog::waves::SeaState sea;
    std::vector<hog::waves::Wave> waves;
    hog::render::DrawnWaves drawn;

    explicit WavySea(double wind, double height = 8.0)
        : eye{glm::normalize(DVec3{0.03, -0.02, 1.0}) * (radius + height)},
          bend{hog::terrain::bend_from(eye, radius, 0.0)} {
        const DVec3 along = glm::normalize(glm::cross(bend.up, north));
        sea = {
            .systems = {hog::waves::grown_sea(along * wind)}, .current = {}, .wind = along * wind};
        waves = hog::waves::waves_at(trains, bend.up, sea);
        drawn = hog::render::drawn_waves_at(trains, waves, sea, eye, bend, noon, DVec3{0.0});
    }

    // Sea level `out` metres from under the eye along the ground, from the eye.
    [[nodiscard]] DVec3 rest(DVec3 out) const {
        const DVec3 flat = out - (bend.up * glm::dot(out, bend.up));
        const DVec3 under = bend.up * radius;
        return (glm::normalize(under + flat) * radius) - eye.metres();
    }
};

// The plane of the sea's surface at the eye, as the sea writes it into the frame's Lighting: how it
// faces, and how far the eye stands above it along that.
glm::vec4 water_at_eye(hog::render::Device& device, const WavySea& sea) {
    auto atmosphere = hog::render::Atmosphere::create(device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT);
    REQUIRE(sky);
    auto ocean =
        hog::render::Ocean::create(device, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, true);
    REQUIRE(ocean);
    const hog::render::Camera camera{.eye = sea.eye,
                                     .forward = glm::normalize(glm::cross(sea.bend.up, north)),
                                     .up = sea.bend.up};
    const hog::render::SkyView seen{
        .camera = camera, .sun = sea.bend.up, .bend = sea.bend, .exposure = 10.0};
    const hog::render::AirBindings air = sky->bindings(0, *atmosphere);
    const OceanView view{
        .camera = camera, .sun = sea.bend.up, .bend = sea.bend, .air = air, .waves = sea.drawn};
    auto read = hog::render::make_buffer(
        device, sizeof(glm::vec4), VK_BUFFER_USAGE_TRANSFER_DST_BIT, hog::render::Memory::Readback);
    REQUIRE(read);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        CHECK(sky->record_frame(commands, extent, 0, *atmosphere, seen));
        CHECK(ocean->lay_water(commands, view, 0, true));
        hog::render::memory_barrier(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy region{
            .srcOffset = hog::render::sea_water_at, .dstOffset = 0, .size = sizeof(glm::vec4)};
        vkCmdCopyBuffer(commands, air.lighting, read->handle(), 1, &region);
        hog::render::memory_barrier(commands, VK_PIPELINE_STAGE_2_COPY_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                                    VK_ACCESS_2_HOST_READ_BIT);
    }));
    REQUIRE(read->invalidate());
    glm::vec4 water{0.0F};
    std::memcpy(&water, read->mapped().data(), sizeof(water));
    // As the CPU reads it back once the frame's done.
    CHECK(sky->eye_above_water(0) == static_cast<double>(water.w));
    CHECK(device.validation_errors() == 0);
    return water;
}

} // namespace

TEST_CASE("the sea finds its surface at the eye, over it or under it", "[render][gpu]") {
    const auto device = test_device();
    for (const double wind : {0.0, 12.0}) {
        for (const double height : {8.0, 0.3, 0.0, -0.3, -6.0}) {
            CAPTURE(wind, height);
            const WavySea sea{wind, height};
            const glm::vec4 water = water_at_eye(*device, sea);
            const DVec3 normal{hog::Vec3{water}};
            // As high as the water stands under the eye, as the waders find it.
            const DVec3 under = sea.rest(DVec3{0.0});
            const double surface =
                probe_waves(*device, sea.drawn, std::span{&under, 1}, 0.01).front().height;
            CHECK_THAT(static_cast<double>(water.w),
                       WithinAbs((height - surface) * glm::dot(normal, sea.bend.up), 2e-3));
            CHECK_THAT(glm::length(normal), WithinAbs(1.0, 1e-5));
            if (wind == 0.0) {
                CHECK(glm::dot(normal, sea.bend.up) > 1.0 - 1e-6);
            } else {
                // Tilted by the waves under the eye, but not so far as to stand on end.
                CHECK(glm::dot(normal, sea.bend.up) < 1.0 - 1e-5);
                CHECK(glm::dot(normal, sea.bend.up) > 0.7);
            }
        }
    }
}

TEST_CASE("the sea's drawn waves move and face it as hog::waves has them", "[render][gpu]") {
    const auto device = test_device();
    const WavySea sea{9.0};
    REQUIRE(sea.drawn.count_left_radius.x == static_cast<float>(hog::render::drawn_waves));
    // The strongest as they show under the eye, and those as hog::waves takes them.
    std::vector<hog::waves::Wave> shown;
    for (const hog::waves::Wave& wave : sea.waves) {
        const double showing =
            std::sqrt(hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
        if (showing > 0.0) {
            shown.push_back({.train = wave.train, .amplitude = wave.amplitude * showing});
        }
    }
    std::vector<hog::waves::Wave> kept =
        hog::waves::strongest(sea.trains, shown, hog::render::drawn_waves).waves;
    for (hog::waves::Wave& wave : kept) {
        wave.amplitude /=
            std::sqrt(hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
    }
    std::mt19937_64 random{2};
    std::uniform_real_distribution<double> spot{-150.0, 150.0};
    std::vector<DVec3> rest;
    rest.reserve(100);
    for (int i = 0; i < 100; ++i) {
        rest.push_back(sea.rest(DVec3{spot(random), spot(random), 0.0}));
    }
    const std::vector<Probed> probed = probe_waves(*device, sea.drawn, rest, 1e-3);
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const hog::waves::Surface surface =
            hog::waves::surface_at(sea.trains, kept, sea.eye.metres() + rest[i], noon);
        CAPTURE(i);
        CHECK(glm::length(probed[i].moved - surface.moved) < 0.01);
        CHECK(glm::dot(probed[i].normal, surface.normal) > 0.9999);
        CHECK(probed[i].roughness == static_cast<double>(sea.drawn.count_left_radius.y));
        CHECK_THAT(probed[i].height,
                   WithinAbs(hog::waves::height_at(sea.trains, kept, sea.eye.metres() + rest[i],
                                                   radius, noon),
                             0.01));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("still water rises only in the waves the wind has grown over it", "[render][gpu]") {
    const auto device = test_device();
    // Inland, with no sea, under a wind of 9 m/s.
    WavySea sea{9.0};
    const double wind = glm::length(sea.sea.wind);
    sea.sea.systems.clear();
    sea.waves.clear();
    sea.drawn = hog::render::drawn_waves_at(sea.trains, sea.waves, sea.sea, sea.eye, sea.bend, noon,
                                            DVec3{0.0});
    // The strongest as they'd run grown full under the wind, as hog::waves takes them.
    const hog::waves::SeaState grown{
        .systems = {hog::waves::grown_sea(sea.sea.wind)}, .current = {}, .wind = sea.sea.wind};
    const auto showing = [&](const hog::waves::Wave& wave) {
        return std::sqrt(
            hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
    };
    std::vector<hog::waves::Wave> shown;
    for (const hog::waves::Wave& wave : hog::waves::waves_at(sea.trains, sea.bend.up, grown)) {
        if (showing(wave) > 0.0) {
            shown.push_back({.train = wave.train, .amplitude = wave.amplitude * showing(wave)});
        }
    }
    const std::vector<hog::waves::Wave> kept =
        hog::waves::strongest(sea.trains, shown, hog::render::drawn_waves).waves;
    REQUIRE(sea.drawn.count_left_radius.x == static_cast<float>(kept.size()));
    std::mt19937_64 random{3};
    std::uniform_real_distribution<double> spot{-100.0, 100.0};
    std::vector<DVec3> rest;
    rest.reserve(40);
    for (int i = 0; i < 40; ++i) {
        rest.push_back(sea.rest(DVec3{spot(random), spot(random), 0.0}));
    }
    const std::vector<hog::Vec3> deep(rest.size(), hog::Vec3{1.0e6F, 0.0F, 0.0F});
    double lower = 0.0;
    for (const float fetch : {40.0F, 400.0F, 4'000.0F}) {
        CAPTURE(fetch);
        std::vector<hog::waves::Wave> fetched;
        for (const hog::waves::Wave& wave : kept) {
            const hog::waves::Train& train = sea.trains[wave.train];
            fetched.push_back({.train = wave.train,
                               .amplitude = wave.amplitude / showing(wave) *
                                            hog::waves::fetched(train.frequency,
                                                                static_cast<double>(fetch), wind)});
        }
        // What's left out of each band, as grown at its middle.
        double left = 0.0;
        for (int band = 0; band < hog::waves::bands; ++band) {
            const double growth = hog::waves::fetched(
                std::sqrt(hog::waves::band_lowest(band) * hog::waves::band_lowest(band + 1)),
                static_cast<double>(fetch), wind);
            left += static_cast<double>(sea.drawn.still_left.at(
                        static_cast<std::size_t>(band / 4))[static_cast<glm::length_t>(band % 4)]) *
                    growth * growth;
        }
        const std::vector<float> fetches(rest.size(), fetch);
        const std::vector<Probed> probed =
            probe_waves(*device, sea.drawn, rest, 1e-3, deep, fetches);
        double heave = 0.0;
        for (std::size_t i = 0; i < rest.size(); ++i) {
            const hog::waves::Surface surface =
                hog::waves::surface_at(sea.trains, fetched, sea.eye.metres() + rest[i], noon);
            CAPTURE(i);
            CHECK(glm::length(probed[i].moved - surface.moved) < 0.005);
            CHECK(glm::dot(probed[i].normal, surface.normal) > 0.9999);
            CHECK_THAT(probed[i].roughness, WithinRel(left, 1e-3));
            heave += probed[i].heave;
        }
        // The further the wind has blown over it, the higher it rises.
        CHECK(heave > lower * 1.5);
        lower = heave;
        // Seen from afar, all of it is roughness, each wave grown as far as its own fetch lets it.
        double all = left;
        for (const hog::waves::Wave& wave : kept) {
            const hog::waves::Train& train = sea.trains[wave.train];
            const double rising = glm::dot(sea.bend.up, train.heading);
            const double slope =
                train.wavenumber * std::sqrt(1.0 - (rising * rising)) * wave.amplitude *
                hog::waves::fetched(train.frequency, static_cast<double>(fetch), wind);
            all += slope * slope / 2.0;
        }
        const Probed far = probe_waves(*device, sea.drawn, std::span{rest}.first(1), 1e4,
                                       std::span{deep}.first(1), std::span{fetches}.first(1))
                               .front();
        CHECK_THAT(far.roughness, WithinRel(all, 1e-3));
        CHECK(glm::length(far.moved) == 0.0);
    }
    // Over the sea, where there's none, it lies flat.
    for (const Probed& probed : probe_waves(*device, sea.drawn, rest, 1e-3, deep)) {
        CHECK(glm::length(probed.moved) < 1e-6);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("over a shore the sea's drawn waves shoal as hog::waves has them", "[render][gpu]") {
    const auto device = test_device();
    // A swell running in at a shore lying east of the eye, and the wind sea.
    WavySea sea{9.0};
    const DVec3 first{hog::Vec3{sea.drawn.first}};
    sea.sea.systems.push_back({.heading = glm::normalize(-first + (north * 0.4)),
                               .height = 1.5,
                               .peak = 0.55,
                               .peakedness = hog::waves::swell_peakedness,
                               .spread = hog::waves::swell_spread});
    sea.waves = hog::waves::waves_at(sea.trains, sea.bend.up, sea.sea);
    sea.drawn = hog::render::drawn_waves_at(sea.trains, sea.waves, sea.sea, sea.eye, sea.bend, noon,
                                            DVec3{0.0});
    std::vector<hog::waves::Wave> shown;
    for (const hog::waves::Wave& wave : sea.waves) {
        const double showing =
            std::sqrt(hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
        if (showing > 0.0) {
            shown.push_back({.train = wave.train, .amplitude = wave.amplitude * showing});
        }
    }
    std::vector<hog::waves::Wave> kept =
        hog::waves::strongest(sea.trains, shown, hog::render::drawn_waves).waves;
    for (hog::waves::Wave& wave : kept) {
        wave.amplitude /=
            std::sqrt(hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
    }
    // From the waterline 300 m east out past the shelf's turn; not so far that the precision
    // floats keep there is what's tested.
    std::vector<DVec3> rest;
    std::vector<hog::Vec3> floors;
    std::vector<hog::Vec3> offshore;
    std::vector<hog::waves::Floor> cpu_floors;
    for (const double west : {295.0, 280.0, 240.0, 150.0, 0.0, -1'500.0}) {
        for (const double across : {-20.0, 35.0}) {
            const double out = 300.0 - west;
            const double depth = std::max(hog::waves::dean_scale * std::cbrt(out * out),
                                          out / hog::waves::shelf_run);
            const double deepening = depth < hog::waves::shelf_depth
                                         ? 2.0 / 3.0 * hog::waves::dean_scale / std::cbrt(out)
                                         : 1.0 / hog::waves::shelf_run;
            rest.push_back(sea.rest((first * west) + (north * across)));
            floors.emplace_back(static_cast<float>(depth), static_cast<float>(-deepening), 0.0F);
            offshore.emplace_back(static_cast<float>(out), -1.0F, 0.0F);
            cpu_floors.push_back(
                {.depth = depth, .deepening = -first * deepening, .out = out, .seaward = -first});
        }
    }
    const std::vector<Probed> probed =
        probe_waves(*device, sea.drawn, rest, 1e-3, floors, {}, offshore);
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const hog::waves::Surface surface = hog::waves::surface_at(
            sea.trains, kept, sea.eye.metres() + rest[i], noon, cpu_floors[i]);
        CAPTURE(i, cpu_floors[i].depth);
        CHECK(glm::length(probed[i].moved - surface.moved) <
              0.01 + (0.01 * glm::length(surface.moved)));
        CHECK(glm::dot(probed[i].normal, surface.normal) > 0.999);
        const double height = hog::waves::height_at(sea.trains, kept, sea.eye.metres() + rest[i],
                                                    radius, noon, cpu_floors[i]);
        CHECK_THAT(probed[i].height, WithinAbs(height, 0.01 + (0.01 * std::abs(height))));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a swell breaking on a beach curls over in the drawn waves as hog::waves has it",
          "[render][gpu]") {
    const auto device = test_device();
    // A big swell and no wind, running in at a shore lying east of the eye.
    WavySea sea{0.0};
    const DVec3 first{hog::Vec3{sea.drawn.first}};
    sea.sea.systems.push_back({.heading = -first,
                               .height = 2.4,
                               .peak = 0.7,
                               .peakedness = hog::waves::swell_peakedness,
                               .spread = hog::waves::swell_spread});
    sea.waves = hog::waves::waves_at(sea.trains, sea.bend.up, sea.sea);
    sea.drawn = hog::render::drawn_waves_at(sea.trains, sea.waves, sea.sea, sea.eye, sea.bend, noon,
                                            DVec3{0.0});
    std::vector<hog::waves::Wave> shown;
    for (const hog::waves::Wave& wave : sea.waves) {
        const double showing =
            std::sqrt(hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
        if (showing > 0.0) {
            shown.push_back({.train = wave.train, .amplitude = wave.amplitude * showing});
        }
    }
    std::vector<hog::waves::Wave> kept =
        hog::waves::strongest(sea.trains, shown, hog::render::drawn_waves).waves;
    for (hog::waves::Wave& wave : kept) {
        wave.amplitude /=
            std::sqrt(hog::waves::showing(glm::dot(sea.bend.up, sea.trains[wave.train].heading)));
    }
    // A 1:10 beach, sampled every half metre over fifty metres a few metres deep.
    constexpr double slope = 0.1;
    std::vector<DVec3> rest;
    std::vector<hog::Vec3> floors;
    std::vector<hog::Vec3> offshore;
    std::vector<hog::waves::Floor> cpu_floors;
    for (int i = 0; i < 100; ++i) {
        const double west = 25.0 - (0.5 * i);
        const double out = 30.0 - west;
        rest.push_back(sea.rest(first * west));
        floors.emplace_back(static_cast<float>(out * slope), static_cast<float>(-slope), 0.0F);
        offshore.emplace_back(static_cast<float>(out), -1.0F, 0.0F);
        cpu_floors.push_back(
            {.depth = out * slope, .deepening = -first * slope, .out = out, .seaward = -first});
    }
    const std::vector<Probed> probed =
        probe_waves(*device, sea.drawn, rest, 1e-3, floors, {}, offshore);
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const hog::waves::Surface surface = hog::waves::surface_at(
            sea.trains, kept, sea.eye.metres() + rest[i], noon, cpu_floors[i]);
        CAPTURE(i, cpu_floors[i].depth);
        CHECK(glm::length(probed[i].moved - surface.moved) <
              0.02 + (0.01 * glm::length(surface.moved)));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the sea's drawn waves carry the tide round the eye", "[render]") {
    // Eight metres over a calm sea at middling latitudes, where the tide runs high.
    const std::vector<hog::waves::Train> trains = hog::waves::planet_trains(5);
    const WorldPosition eye{glm::normalize(DVec3{0.6, 0.5, 0.4}) * (radius + 8.0)};
    const hog::terrain::Bend bend = hog::terrain::bend_from(eye, radius, 0.0);
    const hog::waves::SeaState calm{};
    for (int hour = 0; hour < 8; ++hour) {
        const double turn = 0.3 * hour;
        const std::array pulls{
            hog::waves::Pull{.toward = {std::cos(turn), std::sin(turn), 0.3}, .height = 0.6},
            hog::waves::Pull{.toward = {std::cos(1.1 * turn), -std::sin(1.1 * turn), -0.2},
                             .height = 0.28}};
        const hog::render::DrawnWaves drawn =
            hog::render::drawn_waves_at(trains, {}, calm, eye, bend, noon, DVec3{0.0}, pulls);
        CHECK_THAT(static_cast<double>(drawn.tide.w),
                   WithinAbs(hog::waves::tide_at(bend.up, pulls), 1e-6));
        // As it rises a kilometre off along the ground.
        const DVec3 off = glm::normalize(north - (bend.up * glm::dot(north, bend.up)));
        const double there =
            hog::waves::tide_at(glm::normalize(bend.up + (off * (1'000.0 / radius))), pulls);
        CHECK_THAT(static_cast<double>(drawn.tide.w) +
                       (1'000.0 * glm::dot(DVec3{hog::Vec3{drawn.tide}}, off)),
                   WithinAbs(there, 1e-3));
    }
    CHECK(hog::render::drawn_waves_at(trains, {}, calm, eye, bend, noon, DVec3{0.0}).tide ==
          glm::vec4{0.0F});
}

TEST_CASE("the sea's drawn waves carry their run-up, its swash fixed to the world", "[render]") {
    const WavySea sea{9.0};
    const hog::render::DrawnWaves drawn = sea.drawn;
    // As waves::run_up scales it, from the drawn waves' significant height and peak period.
    CHECK(drawn.run_up.x > 0.0F);
    CHECK(drawn.run_up.y > 0.0F);
    CHECK_THAT(static_cast<double>(drawn.run_up.w),
               WithinRel(static_cast<double>(drawn.run_up.z) / hog::waves::surging, 1e-6));
    // From an eye a few metres on, the swash lies where it did.
    const DVec3 step = glm::normalize(east - (sea.bend.up * glm::dot(east, sea.bend.up))) * 7.0;
    const WorldPosition moved{sea.eye.metres() + step};
    const hog::render::DrawnWaves there =
        hog::render::drawn_waves_at(sea.trains, sea.waves, sea.sea, moved,
                                    hog::terrain::bend_from(moved, radius, 0.0), noon, DVec3{0.0});
    for (const auto& [here, after] : {std::pair{drawn.swash_incident, there.swash_incident},
                                      std::pair{drawn.swash_surging, there.swash_surging}}) {
        const double expected =
            static_cast<double>(here.w) + glm::dot(DVec3{hog::Vec3{here}}, step);
        CHECK_THAT(std::remainder(static_cast<double>(after.w) - expected, 2.0 * std::numbers::pi),
                   WithinAbs(0.0, 1e-3));
    }
}

TEST_CASE("waves too short to see from afar fade into the sea's roughness", "[render][gpu]") {
    const auto device = test_device();
    const WavySea sea{9.0};
    const std::vector<DVec3> rest{sea.rest(DVec3{40.0, 0.0, 0.0})};
    const Probed near = probe_waves(*device, sea.drawn, rest, 1e-3).front();
    const Probed far = probe_waves(*device, sea.drawn, rest, 5.0).front();
    const Probed farther = probe_waves(*device, sea.drawn, rest, 1e4).front();
    const auto all = static_cast<double>(sea.drawn.count_left_radius.y +
                                         sea.drawn.waves.front().amplitude_phase_slope_heave.z);
    CHECK(near.roughness < far.roughness);
    CHECK(far.roughness < farther.roughness);
    // Seen from afar all of it is roughness, and what showed of the shape is gone.
    CHECK(std::abs(farther.roughness - all) < all * 1e-3);
    CHECK(glm::length(farther.moved) == 0.0);
    CHECK(glm::length(far.moved) > 0.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("still water whitens only as far as the wind has grown its waves", "[render][gpu]") {
    const auto device = test_device();
    // Inland, with no sea, under a gale.
    WavySea sea{20.0};
    sea.sea.systems.clear();
    sea.waves.clear();
    sea.drawn = hog::render::drawn_waves_at(sea.trains, sea.waves, sea.sea, sea.eye, sea.bend, noon,
                                            DVec3{0.0});
    REQUIRE(sea.drawn.foam.z == 0.0F);
    const hog::waves::Whitecaps open = hog::waves::whitecaps(20.0);
    // Seen from afar, each footprint's as white as the water is on the whole there.
    const std::vector<DVec3> far{sea.rest(DVec3{0.0, 0.0, 0.0}), sea.rest(DVec3{900.0, 0.0, 0.0})};
    const std::vector<hog::Vec3> deep(far.size(), hog::Vec3{1.0e6F, 0.0F, 0.0F});
    const auto white_at = [&](float fetch) {
        const std::vector<float> fetches(far.size(), fetch);
        return probe_waves(*device, sea.drawn, far, 1e5, deep, fetches);
    };
    // Grown full, as white as the open sea under the same wind.
    for (const Probed& probed : white_at(1.0e6F)) {
        CHECK_THAT(probed.breaking, WithinRel(open.breaking, 0.02));
        CHECK_THAT(probed.left, WithinRel(open.left, 0.02));
    }
    // Less so the younger it is, as hog::waves has it.
    for (const float fetch : {3'000.0F, 30.0F}) {
        CAPTURE(fetch);
        const hog::waves::Whitecaps young = hog::waves::whitecaps(20.0, static_cast<double>(fetch));
        CHECK(young.breaking < open.breaking / 2.0);
        for (const Probed& probed : white_at(fetch)) {
            CHECK_THAT(probed.breaking, WithinRel(young.breaking, 0.02));
            CHECK_THAT(probed.left, WithinRel(young.left, 0.02));
        }
    }
    // Close by, each place is white or not, as much of it as there should be.
    std::mt19937_64 random{4};
    std::uniform_real_distribution<double> spot{-3'000.0, 3'000.0};
    std::vector<DVec3> rest;
    rest.reserve(4'000);
    for (int i = 0; i < 4'000; ++i) {
        rest.push_back(sea.rest(DVec3{spot(random), spot(random), 0.0}));
    }
    const std::vector<hog::Vec3> floors(rest.size(), hog::Vec3{1.0e6F, 0.0F, 0.0F});
    const std::vector<float> fetches(rest.size(), 1.0e6F);
    double mean_breaking = 0.0;
    for (const Probed& probed : probe_waves(*device, sea.drawn, rest, 1e-3, floors, fetches)) {
        mean_breaking += probed.breaking;
    }
    CHECK_THAT(mean_breaking / static_cast<double>(rest.size()), WithinRel(open.breaking, 0.1));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("crests break white over as much of the sea as the wind breaks", "[render][gpu]") {
    const auto device = test_device();
    const WavySea sea{20.0};
    const auto breaking = static_cast<double>(sea.drawn.foam.z);
    const auto left = static_cast<double>(sea.drawn.foam.w);
    REQUIRE(breaking > 0.01);
    REQUIRE(left > breaking);
    // Close by, each place is white or not; over kilometres, as much as the wind makes white.
    std::mt19937_64 random{3};
    std::uniform_real_distribution<double> spot{-3'000.0, 3'000.0};
    std::vector<DVec3> rest;
    rest.reserve(4'000);
    for (int i = 0; i < 4'000; ++i) {
        rest.push_back(sea.rest(DVec3{spot(random), spot(random), 0.0}));
    }
    double mean_breaking = 0.0;
    double mean_left = 0.0;
    double coarse = 0.0;
    double coarse_squared = 0.0;
    int unsure = 0;
    for (const Probed& probed : probe_waves(*device, sea.drawn, rest, 1e-3)) {
        mean_breaking += probed.breaking;
        mean_left += probed.left;
        coarse += probed.coarse;
        coarse_squared += probed.coarse * probed.coarse;
        unsure += probed.breaking > 0.05 && probed.breaking < 0.95 ? 1 : 0;
        if (probed.breaking + probed.left > 0.01) {
            CHECK_THAT(probed.seen, WithinRel(1.0, 1e-3));
        }
    }
    const auto count = static_cast<double>(rest.size());
    CHECK_THAT(mean_breaking / count, WithinRel(breaking, 0.1));
    CHECK_THAT(mean_left / count, WithinRel(left, 0.15));
    CHECK(std::abs(coarse / count) < 0.1);
    CHECK_THAT(coarse_squared / count, WithinRel(1.0, 0.1));
    // Whitecaps fade out over their edges, which are less of them than what's white through.
    CHECK(static_cast<double>(unsure) < count * breaking);
    // From afar, each footprint is that much white.
    const std::vector<DVec3> far{sea.rest(DVec3{0.0, 0.0, 0.0}), sea.rest(DVec3{900.0, 0.0, 0.0})};
    for (const Probed& probed : probe_waves(*device, sea.drawn, far, 1e5)) {
        CHECK(probed.seen == 0.0);
        CHECK_THAT(probed.breaking, WithinRel(breaking, 0.02));
        CHECK_THAT(probed.left, WithinRel(left, 0.02));
    }
    // A breeze whitens next to nothing.
    const WavySea breeze{2.0};
    CHECK(breeze.drawn.foam.z < 1e-4F);
    int white = 0;
    int fine = 0;
    for (const Probed& probed : probe_waves(*device, breeze.drawn, rest, 1e-3)) {
        white += probed.breaking + probed.left > 0.5 ? 1 : 0;
        fine += probed.seen > 0.0 ? 1 : 0;
    }
    CHECK(white < 4);
    // The fine pattern is only taken where there might be foam.
    CHECK(static_cast<double>(fine) < count / 10.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("in the surf, as much of the sea breaks white as its waves break, wind or none",
          "[render][gpu]") {
    const auto device = test_device();
    // A swell running in over a shore under a breeze too light to whiten the sea.
    WavySea sea{3.0};
    const DVec3 first{hog::Vec3{sea.drawn.first}};
    sea.sea.systems.push_back({.heading = glm::normalize(first + (north * 0.4)),
                               .height = 1.5,
                               .peak = 0.55,
                               .peakedness = hog::waves::swell_peakedness,
                               .spread = hog::waves::swell_spread});
    sea.waves = hog::waves::waves_at(sea.trains, sea.bend.up, sea.sea);
    sea.drawn = hog::render::drawn_waves_at(sea.trains, sea.waves, sea.sea, sea.eye, sea.bend, noon,
                                            DVec3{0.0});
    REQUIRE(sea.drawn.foam.z < 1e-4F);
    std::mt19937_64 random{4};
    std::uniform_real_distribution<double> spot{-1'500.0, 1'500.0};
    std::vector<DVec3> rest;
    rest.reserve(4'000);
    for (int i = 0; i < 4'000; ++i) {
        rest.push_back(sea.rest(DVec3{spot(random), spot(random), 0.0}));
    }
    for (const double depth : {0.8, 1.5, 3.0, 6.0}) {
        CAPTURE(depth);
        const double deepening =
            2.0 / 3.0 * hog::waves::dean_scale / std::sqrt(depth / hog::waves::dean_scale);
        const std::vector<hog::Vec3> floors(
            rest.size(),
            hog::Vec3{static_cast<float>(depth), static_cast<float>(-deepening), 0.0F});
        double breaking = 0.0;
        double left = 0.0;
        double expected_breaking = 0.0;
        double expected_left = 0.0;
        for (const Probed& probed : probe_waves(*device, sea.drawn, rest, 1e-3, floors)) {
            const double height = std::sqrt(8.0) * probed.heave;
            CHECK_THAT(probed.surf, WithinRel(hog::waves::breaking_waves(height, depth), 1e-3) ||
                                        WithinAbs(0.0, 1e-9));
            const hog::waves::Whitecaps surf = hog::waves::surf(height, depth);
            breaking += probed.breaking;
            left += probed.left;
            expected_breaking += surf.breaking;
            expected_left += surf.left;
        }
        // Close by, each place is white or not; over the shore, as much as breaks. Where little
        // does, only near it: a swell's few trains rise less often far over their mean than a
        // normal sea.
        const auto count = static_cast<double>(rest.size());
        CHECK(expected_breaking > 0.0);
        CHECK_THAT(breaking / count, WithinRel(expected_breaking / count, 0.1) ||
                                         WithinAbs(expected_breaking / count, 0.003));
        CHECK_THAT(left / count, WithinRel(expected_left / count, 0.1) ||
                                     WithinAbs(expected_left / count, 0.003));
        // From afar, each footprint is that much white.
        const std::vector<DVec3> far{rest.front(), rest.back()};
        const std::vector<hog::Vec3> far_floors(far.size(), floors.front());
        for (const Probed& probed : probe_waves(*device, sea.drawn, far, 1e5, far_floors)) {
            // Or as much as the breeze whitens, if more.
            const hog::waves::Whitecaps surf =
                hog::waves::surf(std::sqrt(8.0) * probed.heave, depth);
            CHECK(probed.seen == 0.0);
            CHECK_THAT(
                probed.breaking,
                WithinRel(std::max(surf.breaking, static_cast<double>(sea.drawn.foam.z)), 0.02));
            CHECK_THAT(probed.left,
                       WithinRel(std::max(surf.left, static_cast<double>(sea.drawn.foam.w)), 0.02));
        }
    }
    // Out where it's deep, the breeze whitens next to nothing.
    int white = 0;
    for (const Probed& probed : probe_waves(*device, sea.drawn, rest, 1e-3)) {
        white += probed.breaking + probed.left > 0.5 ? 1 : 0;
        CHECK(probed.surf == 0.0);
    }
    CHECK(white < 4);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the sea's foam drifts with the water at its surface", "[render][gpu]") {
    const auto device = test_device();
    const WavySea sea{20.0};
    const DVec3 drifted = glm::normalize(glm::cross(sea.bend.up, east)) * 37.0;
    const hog::render::DrawnWaves moved = hog::render::drawn_waves_at(
        sea.trains, sea.waves, sea.sea, sea.eye, sea.bend, noon, drifted);
    std::vector<DVec3> there;
    std::vector<DVec3> before;
    for (int i = 0; i < 20; ++i) {
        const DVec3 out{(i * 7.0) - 60.0, (i % 5) * 11.0, 0.0};
        there.push_back(sea.rest(out));
        before.push_back(there.back() - drifted);
    }
    const std::vector<Probed> now = probe_waves(*device, moved, there, 1e-3);
    const std::vector<Probed> then = probe_waves(*device, sea.drawn, before, 1e-3);
    for (std::size_t i = 0; i < now.size(); ++i) {
        CAPTURE(i);
        CHECK_THAT(now[i].coarse, WithinAbs(then[i].coarse, 1e-3));
        if (now[i].seen > 0.0 && then[i].seen > 0.0) {
            CHECK_THAT(now[i].fine, WithinAbs(then[i].fine, 1e-3));
        }
    }
    CHECK(device->validation_errors() == 0);
}

namespace {

// What the ocean's shaders find over the plane of `frame` round an eye 5 m up at its anchor.
struct ShoreFound {
    double depth;
    double still;
    double fetch;
    double level;
    double covered; // how far inside the grids
};

// Of fields over the plane of `frame`, each by metres east and north on it: of the sea's depths,
// still water's, the level it's kept to and the fetch towards each heading. What the ocean's
// shaders find under each of `points`, the fetch for a wind blowing towards `wind`.
std::vector<ShoreFound> probe_shore(hog::render::Device& device,
                                    const hog::terrain::ShoreFrame& frame, const auto& depth_at,
                                    const auto& still_at, const auto& level_at,
                                    const auto& fetch_at, DVec3 wind,
                                    std::span<const hog::DVec2> points) {
    using hog::terrain::fetch_headings;
    using hog::terrain::shore_size;
    using hog::terrain::shore_spacing;
    std::vector<float> depths;
    std::vector<float> still;
    std::vector<float> levels;
    std::vector<float> fetches;
    const auto each = [&](double spacing, const auto& at) {
        const double first = (0.5 - (shore_size / 2.0)) * spacing;
        for (int row = 0; row < shore_size; ++row) {
            for (int column = 0; column < shore_size; ++column) {
                at(hog::DVec2{first + (column * spacing), first + (row * spacing)});
            }
        }
    };
    for (const double spacing : shore_spacing) {
        each(spacing, [&](hog::DVec2 at) {
            depths.push_back(static_cast<float>(depth_at(at)));
            still.push_back(static_cast<float>(still_at(at)));
            levels.push_back(static_cast<float>(level_at(at)));
        });
        for (int heading = 0; heading < fetch_headings; ++heading) {
            each(spacing, [&](hog::DVec2 at) {
                fetches.push_back(static_cast<float>(fetch_at(at, heading)));
            });
        }
    }
    const auto layers = static_cast<std::uint32_t>(shore_spacing.size());
    auto image =
        hog::render::make_streamed_image(device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         {.width = shore_size, .height = shore_size}, 1, layers);
    REQUIRE(image);
    auto fetch_image = hog::render::make_streamed_image(device, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                        {.width = shore_size, .height = shore_size},
                                                        1, layers * hog::render::reach_layers);
    REQUIRE(fetch_image);
    const std::vector<std::uint16_t> texels = hog::render::shore_texels(depths, still, levels);
    const std::vector<std::uint16_t> fetch_texels = hog::render::reach_texels(fetches, {});
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        REQUIRE(image->update(commands, 0, std::as_bytes(std::span{texels})));
        REQUIRE(fetch_image->update(commands, 0, std::as_bytes(std::span{fetch_texels})));
    }));
    auto sampler = hog::render::make_sampler(device, VK_FILTER_LINEAR);
    REQUIRE(sampler);

    const DVec3 eye = frame.anchor * (radius + 5.0);
    std::vector<glm::vec4> asked{
        glm::vec4{hog::Vec3{(frame.anchor * radius) - eye},
                  static_cast<float>(1.0 / shore_spacing[0])},
        glm::vec4{hog::Vec3{frame.east}, static_cast<float>(1.0 / shore_spacing[1])},
        glm::vec4{hog::Vec3{frame.north}, static_cast<float>(1.0 / shore_spacing[2])},
        glm::vec4{hog::Vec3{wind}, 0.0F}};
    for (const hog::DVec2 point : points) {
        const DVec3 rest = (hog::terrain::shore_direction(frame, radius, point) * radius) - eye;
        asked.emplace_back(hog::Vec3{rest}, static_cast<float>(radius));
    }
    constexpr auto binding = [](std::uint32_t at, VkDescriptorType type) {
        return VkDescriptorSetLayoutBinding{.binding = at,
                                            .descriptorType = type,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = nullptr};
    };
    const std::array bindings{binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                              binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                              binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                              binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};
    auto probe = hog::render::make_compute_pipeline(
        device, {.spirv = hog::shaders::shore_probe, .descriptors = bindings});
    REQUIRE(probe);
    auto questions = hog::render::make_filled_buffer(device, std::as_bytes(std::span{asked}),
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(questions);
    std::vector<std::array<float, 4>> found(points.size() * 2);
    auto answers =
        hog::render::make_buffer(device, std::span{found}.size_bytes(),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(answers);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe->handle());
        hog::render::push_sampled_image(commands, *probe, 0, image->image().view(),
                                        sampler->handle());
        hog::render::push_storage_buffer(commands, *probe, 1, questions->handle());
        hog::render::push_storage_buffer(commands, *probe, 2, answers->handle());
        hog::render::push_sampled_image(commands, *probe, 3, fetch_image->image().view(),
                                        sampler->handle());
        vkCmdDispatch(commands, static_cast<std::uint32_t>((points.size() + 63) / 64), 1, 1);
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
    std::vector<ShoreFound> shore_found;
    shore_found.reserve(found.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& at = found[2 * i];
        shore_found.push_back({.depth = static_cast<double>(at[0]),
                               .still = static_cast<double>(at[1]),
                               .fetch = static_cast<double>(at[2]),
                               .level = static_cast<double>(at[3]),
                               .covered = static_cast<double>(found[(2 * i) + 1][0])});
    }
    return shore_found;
}

} // namespace

TEST_CASE("the sea's shaders find the depths round the eye", "[render][ocean][gpu]") {
    const auto device = test_device();
    const hog::terrain::ShoreFrame frame =
        hog::terrain::shore_frame(glm::normalize(DVec3{0.2, -0.4, 0.9}), radius);
    // Rising to the east and falling to the north, which filtering keeps as it is.
    const auto depth_at = [](hog::DVec2 at) { return 20.0 + (0.004 * at.x) - (0.002 * at.y); };
    // Fine, where the fine grid fades into the coarse, coarse, where that fades into the
    // coarsest, coarsest, fading out of it, and beyond.
    const std::array<hog::DVec2, 8> points{hog::DVec2{0.0, 0.0},      hog::DVec2{300.0, -420.0},
                                           hog::DVec2{-950.0, 100.0}, hog::DVec2{4000.0, -3000.0},
                                           hog::DVec2{7000.0, 0.0},   hog::DVec2{0.0, -20000.0},
                                           hog::DVec2{60000.0, 0.0},  hog::DVec2{-70000.0, 5.0}};
    const std::vector<ShoreFound> found = probe_shore(
        *device, frame, depth_at, [](hog::DVec2) { return hog::terrain::shore_highest; },
        [](hog::DVec2) { return 0.0; }, [](hog::DVec2, int) { return 0.0; }, frame.east, points);
    REQUIRE(found.size() == points.size());
    // Half floats keep about three digits.
    for (std::size_t i = 0; i < 6; ++i) {
        CAPTURE(i);
        CHECK_THAT(found[i].depth, WithinAbs(depth_at(points[i]), 0.001 * depth_at(points[i])));
    }
    const double inside = (1.0 - (60000.0 / 65536.0)) / 0.2;
    CHECK_THAT(found[6].depth, WithinAbs(1000.0 + ((depth_at(points[6]) - 1000.0) * inside), 1.0));
    CHECK(found[7].depth == 1000.0);
    // The ground draws lakes beyond the grids, fading them in as the grids fade out.
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(found[i].covered == 1.0);
    }
    CHECK_THAT(found[6].covered, WithinAbs(inside, 1e-4));
    CHECK(found[7].covered == 0.0);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the sea's shaders find still water round the eye, and the fetch over it",
          "[render][ocean][gpu]") {
    const auto device = test_device();
    const hog::terrain::ShoreFrame frame =
        hog::terrain::shore_frame(glm::normalize(DVec3{-0.3, 0.5, 0.8}), radius);
    const auto still_at = [](hog::DVec2 at) { return 3.0 + (0.001 * at.x); };
    const auto fetch_at = [](hog::DVec2 at, int heading) {
        return (100.0 * (heading + 1)) + (0.05 * at.y);
    };
    const std::array<hog::DVec2, 3> points{hog::DVec2{0.0, 0.0}, hog::DVec2{300.0, -420.0},
                                           hog::DVec2{-2000.0, 3000.0}};
    // Blowing a third of the way from one heading to the next, and across east.
    const auto towards = [&](double degrees) {
        const double turned = degrees * std::numbers::pi / 180.0;
        return (frame.east * std::cos(turned)) + (frame.north * std::sin(turned));
    };
    for (const auto& [degrees, before, after] : {std::tuple{15.0, 0, 1}, std::tuple{330.0, 7, 0}}) {
        CAPTURE(degrees);
        const std::vector<ShoreFound> found = probe_shore(
            *device, frame, [](hog::DVec2) { return hog::terrain::shore_highest; }, still_at,
            [](hog::DVec2) { return 0.0; }, fetch_at, towards(degrees), points);
        const double between = std::fmod(degrees, 45.0) / 45.0;
        for (std::size_t i = 0; i < points.size(); ++i) {
            CAPTURE(i);
            CHECK_THAT(found[i].still, WithinAbs(still_at(points[i]), 0.01));
            CHECK_THAT(found[i].fetch, WithinAbs(std::lerp(fetch_at(points[i], before),
                                                           fetch_at(points[i], after), between),
                                                 0.5));
        }
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("the sea's shaders find a lake's level high in the mountains to a hair",
          "[render][ocean][gpu]") {
    const auto device = test_device();
    const hog::terrain::ShoreFrame frame =
        hog::terrain::shore_frame(glm::normalize(DVec3{0.6, 0.1, -0.7}), radius);
    // Not a level lake, so filtering between texels is seen to keep it.
    const auto level_at = [](hog::DVec2 at) { return 2'345.678 + (0.0003 * at.x); };
    const std::array<hog::DVec2, 3> points{hog::DVec2{0.0, 0.0}, hog::DVec2{301.0, -419.0},
                                           hog::DVec2{-2'003.0, 3'001.0}};
    const std::vector<ShoreFound> found = probe_shore(
        *device, frame, [](hog::DVec2) { return hog::terrain::shore_highest; },
        [](hog::DVec2) { return 4.0; }, level_at, [](hog::DVec2, int) { return 100.0; }, frame.east,
        points);
    for (std::size_t i = 0; i < points.size(); ++i) {
        CAPTURE(i);
        CHECK_THAT(found[i].level, WithinAbs(level_at(points[i]), 0.01));
        CHECK_THAT(found[i].still, WithinAbs(4.0, 1e-3));
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("rivers' plumes ride alongside how far offshore the water lies", "[render][ocean]") {
    constexpr auto texels = std::size_t{hog::terrain::shore_size} * hog::terrain::shore_size;
    constexpr auto layers = hog::terrain::shore_spacing.size();
    std::vector<float> plumes(layers * texels, 0.0F);
    plumes[5] = 0.25F;
    plumes[texels + 7] = 1.0F;
    const std::vector<std::uint16_t> halves = hog::render::reach_texels({}, {}, plumes);
    const std::vector<std::uint16_t> none = hog::render::reach_texels({}, {});
    const auto plume_at = [&](const std::vector<std::uint16_t>& from, std::size_t layer,
                              std::size_t at) {
        const std::size_t into =
            (layer * hog::render::reach_layers) + hog::render::reach_layers - 1;
        return glm::unpackHalf1x16(from.at((((into * texels) + at) * 4) + 3));
    };
    CHECK(plume_at(halves, 0, 5) == 0.25F);
    CHECK(plume_at(halves, 1, 7) == 1.0F);
    CHECK(plume_at(halves, 0, 7) == 0.0F);
    CHECK(plume_at(halves, 2, 5) == 0.0F);
    // None mixed in without them.
    for (std::size_t layer = 0; layer < layers; ++layer) {
        CHECK(plume_at(none, layer, 5) == 0.0F);
    }
}
