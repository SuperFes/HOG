#include "test_device.hpp"

#include <hog/jobs/job_system.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/blocks.hpp>
#include <hog/render/clouds.hpp>
#include <hog/render/device.hpp>
#include <hog/render/environment_probe.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/weather_maps.hpp>
#include <hog/shaders/cube_probe.hpp>
#include <hog/terrain/block_mesh.hpp>
#include <hog/terrain/material.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::render::testing::test_device;

namespace {

constexpr VkFormat colour_format = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
constexpr double earth = 6'371'000.0;
constexpr double degree = std::numbers::pi / 180.0;

// What a probe's cube holds along each of `directions`, from `device`.
std::vector<std::array<float, 4>> sampled(hog::render::Device& device,
                                          const hog::render::EnvironmentProbe& probe,
                                          const std::vector<DVec3>& directions) {
    constexpr auto binding = [](std::uint32_t at, VkDescriptorType type) {
        return VkDescriptorSetLayoutBinding{.binding = at,
                                            .descriptorType = type,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = nullptr};
    };
    const std::array bindings{binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                              binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                              binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
    auto pipeline = hog::render::make_compute_pipeline(
        device, {.spirv = hog::shaders::cube_probe, .descriptors = bindings});
    REQUIRE(pipeline);
    std::vector<std::array<float, 4>> asked;
    asked.reserve(directions.size());
    for (const DVec3 direction : directions) {
        asked.push_back({static_cast<float>(direction.x), static_cast<float>(direction.y),
                         static_cast<float>(direction.z), 0.0F});
    }
    auto questions = hog::render::make_filled_buffer(device, std::as_bytes(std::span{asked}),
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(questions);
    auto answers =
        hog::render::make_buffer(device, std::span{asked}.size_bytes(),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(answers);
    auto sampler = hog::render::make_sampler(device, VK_FILTER_LINEAR);
    REQUIRE(sampler);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->handle());
        hog::render::push_sampled_image(commands, *pipeline, 0, probe.cube().view(),
                                        sampler->handle());
        hog::render::push_storage_buffer(commands, *pipeline, 1, questions->handle());
        hog::render::push_storage_buffer(commands, *pipeline, 2, answers->handle());
        vkCmdDispatch(commands, static_cast<std::uint32_t>((asked.size() + 63) / 64), 1, 1);
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
    std::vector<std::array<float, 4>> found(asked.size());
    std::memcpy(found.data(), answers->mapped().data(), std::span{found}.size_bytes());
    return found;
}

double brightness(const std::array<float, 4>& colour) {
    return static_cast<double>(colour[0] + colour[1] + colour[2]);
}

} // namespace

TEST_CASE("an image made with layer views has a view of each layer", "[render][gpu]") {
    const auto device = test_device();
    auto image = hog::render::make_image(*device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                                                   .extent = {.width = 4, .height = 4},
                                                   .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                   .layers = 3,
                                                   .layer_views = true});
    REQUIRE(image);
    std::vector<VkImageView> views{image->view()};
    for (std::uint32_t layer = 0; layer < 3; ++layer) {
        CHECK(image->layer_view(layer) != VK_NULL_HANDLE);
        views.push_back(image->layer_view(layer));
    }
    std::ranges::sort(views);
    CHECK(std::ranges::adjacent_find(views) == views.end());
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a probe's cube holds the sky each way round the eye, laid out by the ground's frame",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    auto atmosphere = hog::render::Atmosphere::create(*device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(*device, colour_format, depth_format);
    REQUIRE(sky);
    auto probe = hog::render::EnvironmentProbe::create(*device, colour_format, depth_format, 64);
    REQUIRE(probe);

    // At the pole, where the ground's frame has east along +x and north along +y; the sun low in
    // the north-east.
    const DVec3 up{0.0, 0.0, 1.0};
    const DVec3 east{1.0, 0.0, 0.0};
    const auto low = [&](double x, double y, double elevation) {
        return glm::normalize((glm::normalize(DVec3{x, y, 0.0}) * std::cos(elevation * degree)) +
                              (up * std::sin(elevation * degree)));
    };
    const hog::render::SkyView view{
        .camera = {.eye = hog::WorldPosition{up * (earth + 10.0)}, .forward = east, .up = up},
        .sun = low(1.0, 1.0, 5.0),
        .bend = {.up = up, .eye_height = 10.0, .radius = earth, .faked = earth},
        .exposure = 1.0};
    REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
        CHECK(sky->record_frame(commands, {.width = 64, .height = 64}, 0, *atmosphere, view));
        CHECK(probe->record(commands, 0, *atmosphere, *sky, {.sky = view, .around = {}}));
    }));

    const std::vector<DVec3> ways{
        low(1.0, 1.0, 5.0), low(1.0, -1.0, 5.0), low(-1.0, 1.0, 5.0), low(-1.0, -1.0, 5.0), up,
        low(1.0, 1.0, 60.0)};
    std::vector<DVec3> in_cube;
    in_cube.reserve(ways.size());
    for (const DVec3 way : ways) {
        in_cube.push_back(hog::render::probe_direction(way, up, east));
    }
    const std::vector<std::array<float, 4>> found = sampled(*device, *probe, in_cube);
    const double north_east = brightness(found[0]);
    const double south_east = brightness(found[1]);
    const double north_west = brightness(found[2]);
    const double south_west = brightness(found[3]);
    CAPTURE(north_east, south_east, north_west, south_west, brightness(found[4]),
            brightness(found[5]));
    // Brightest towards the sun, and alike either side of it; mirrored the wrong way, a pair
    // would swap.
    CHECK(north_east > south_east * 1.5);
    CHECK(north_east > north_west * 1.5);
    CHECK(north_east > south_west * 1.5);
    CHECK(std::abs(south_east - north_west) < 0.01 * north_east);
    // The sky overhead is drawn into its face, and towards the sun brighter.
    CHECK(brightness(found[4]) > 0.0);
    CHECK(brightness(found[5]) > brightness(found[4]));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a probe's cube holds the clouds over the eye, for water to mirror and be seen out of",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    auto atmosphere = hog::render::Atmosphere::create(*device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(*device, colour_format, depth_format);
    REQUIRE(sky);
    hog::jobs::JobSystem jobs{2};
    auto clouds =
        hog::render::Clouds::create(*device, colour_format, jobs, hog::render::CloudQuality::Low);
    REQUIRE(clouds);
    constexpr std::uint32_t map_size = 16;
    auto maps = hog::render::WeatherMaps::create(*device, map_size);
    REQUIRE(maps);
    // Overcast from 2 km to 3 km: a full cover holding 1 mm, in layers 250 m deep.
    const auto filled = [](std::size_t size, std::array<std::uint8_t, 4> texel, std::size_t span) {
        std::vector<std::byte> bytes(size);
        for (std::size_t at = 0; at < size; ++at) {
            bytes[at] = static_cast<std::byte>(texel.at(at % span));
        }
        return bytes;
    };
    const std::vector<std::byte> weather = filled(maps->map_bytes(), {255, 64, 0, 0}, 4);
    const std::vector<std::byte> dry = filled(maps->map_bytes(), {0, 31, 47, 0}, 4);
    const std::vector<std::byte> still(maps->drift_bytes());
    std::vector<std::byte> layers(maps->layer_bytes() * hog::render::most_cloud_layers);
    for (std::size_t layer = 8; layer < 12; ++layer) {
        const std::vector<std::byte> one = filled(maps->layer_bytes(), {255, 32, 0, 0}, 2);
        std::ranges::copy(one, layers.begin() + static_cast<std::ptrdiff_t>(layer * one.size()));
    }
    REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
        CHECK(maps->update(commands, 0,
                           {.weather_before = weather,
                            .weather_after = weather,
                            .sky_before = dry,
                            .sky_after = dry,
                            .drift_before = still,
                            .drift_after = still,
                            .layers_before = layers,
                            .layers_after = layers,
                            .air = {.count = hog::render::most_cloud_layers,
                                    .bottom = 0.0,
                                    .thickness = 250.0,
                                    .ratio = 1.0}}));
    }));

    const DVec3 up{0.0, 0.0, 1.0};
    const DVec3 east{1.0, 0.0, 0.0};
    const hog::render::SkyView view{
        .camera = {.eye = hog::WorldPosition{up * (earth + 10.0)}, .forward = east, .up = up},
        .sun = glm::normalize(east + up),
        .bend = {.up = up, .eye_height = 10.0, .radius = earth, .faked = earth},
        .exposure = 1.0,
        .weather = maps->bindings(1.0)};
    // Overhead, as the clear sky and as the clouds have it; with the frame's own eye in the water,
    // as when the probe is drawn from over it.
    const auto overhead = [&](bool clouded, bool in_water = false) {
        auto probe =
            hog::render::EnvironmentProbe::create(*device, colour_format, depth_format, 64);
        REQUIRE(probe);
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            CHECK(sky->record_frame(commands, {.width = 64, .height = 64}, 0, *atmosphere, view));
            const hog::render::CloudsView seen{.camera = view.camera,
                                               .bend = view.bend,
                                               .air = sky->bindings(0, *atmosphere, VK_NULL_HANDLE),
                                               .weather = view.weather};
            if (in_water) {
                const glm::vec4 drowned{hog::Vec3{up}, -1e9F};
                vkCmdUpdateBuffer(commands, seen.air.lighting, hog::render::sea_water_at,
                                  sizeof(drowned), &drowned);
                hog::render::memory_barrier(
                    commands, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_UNIFORM_READ_BIT);
            }
            CHECK(probe->record(commands, 0, *atmosphere, *sky,
                                {.sky = view,
                                 .around = {},
                                 .clouds = clouded ? &*clouds : nullptr,
                                 .clouds_view = seen}));
        }));
        return sampled(*device, *probe, {hog::render::probe_direction(up, up, east)}).front();
    };
    const std::array<float, 4> clear = overhead(false);
    const std::array<float, 4> clouded = overhead(true);
    const std::array<float, 4> from_water = overhead(true, true);
    CAPTURE(clear[0], clear[1], clear[2], clouded[0], clouded[1], clouded[2], from_water[0],
            from_water[2]);
    // The clear sky's blue; the cloud over it grey, as red nearly as it's blue.
    CHECK(clear[2] > 1.5F * clear[0]);
    CHECK(clouded[2] < 1.3F * clouded[0]);
    CHECK(from_water[2] < 1.3F * from_water[0]);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a probe's cube holds what stands round the eye over the sky",
          "[render][gpu][atmosphere]") {
    const auto device = test_device();
    auto atmosphere = hog::render::Atmosphere::create(*device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(*device, colour_format, depth_format);
    REQUIRE(sky);
    auto probe = hog::render::EnvironmentProbe::create(*device, colour_format, depth_format, 64);
    REQUIRE(probe);
    // Two alike, as the blocks' tiles are always an array of them.
    const hog::terrain::MaterialImage tile =
        hog::terrain::material_image({.colour = DVec3{0.0, 1.0, 0.0}}, 64);
    const std::vector<hog::terrain::MaterialImage> green{tile, tile};
    auto blocks = hog::render::Blocks::create(*device, colour_format, depth_format, green);
    REQUIRE(blocks);

    // At the pole, a wall forty metres high and wide twenty metres east of the eye, facing it.
    const DVec3 up{0.0, 0.0, 1.0};
    const DVec3 east{1.0, 0.0, 0.0};
    const DVec3 north{0.0, 1.0, 0.0};
    const hog::WorldPosition eye{up * (earth + 10.0)};
    const auto wall = std::make_shared<hog::terrain::BlockMesh>();
    wall->origin = eye;
    for (const auto& [across, high] : std::array<std::pair<double, double>, 4>{
             {{-20.0, -20.0}, {20.0, -20.0}, {20.0, 20.0}, {-20.0, 20.0}}}) {
        wall->vertices.push_back(
            {.position = hog::Vec3{(east * 20.0) + (north * across) + (up * high)},
             .normal = hog::Vec3{-east},
             .tiles = {0, 0, 0},
             .corner = 0});
    }
    wall->indices = {0, 2, 1, 0, 3, 2};
    hog::terrain::mix_corners(*wall);
    blocks->show(wall);

    const hog::render::SkyView view{
        .camera = {.eye = eye, .forward = east, .up = up},
        .sun = glm::normalize(DVec3{0.0, 1.0, 1.0}),
        .bend = {.up = up, .eye_height = 10.0, .radius = earth, .faked = earth},
        .exposure = 1.0};
    REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
        CHECK(sky->record_frame(commands, {.width = 64, .height = 64}, 0, *atmosphere, view));
        CHECK(blocks->prepare(0));
        const auto around = [&](VkCommandBuffer drawing, const hog::render::Camera& camera,
                                VkExtent2D extent) -> hog::Result<> {
            blocks->draw_prepared(drawing, extent,
                                  {.camera = camera, .sun = view.sun, .bend = view.bend}, 0);
            return {};
        };
        CHECK(probe->record(commands, 0, *atmosphere, *sky, {.sky = view, .around = around}));
    }));
    const std::vector<std::array<float, 4>> found =
        sampled(*device, *probe,
                std::vector<DVec3>{hog::render::probe_direction(east, up, east),
                                   hog::render::probe_direction(-east, up, east)});
    CAPTURE(found[0][0], found[0][1], found[0][2], found[1][0], found[1][1], found[1][2]);
    // The wall to the east is green; the sky to the west is bluer than green.
    CHECK(found[0][1] > 2.0F * found[0][0]);
    CHECK(found[0][1] > 2.0F * found[0][2]);
    CHECK(found[1][2] > found[1][1]);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a probe draws one face a time after the first, keeping the others", "[render][gpu]") {
    const auto device = test_device();
    auto atmosphere = hog::render::Atmosphere::create(*device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(*device, colour_format, depth_format);
    REQUIRE(sky);
    auto probe = hog::render::EnvironmentProbe::create(*device, colour_format, depth_format, 64);
    REQUIRE(probe);
    const hog::terrain::MaterialImage tile =
        hog::terrain::material_image({.colour = DVec3{0.0, 1.0, 0.0}}, 64);
    const std::vector<hog::terrain::MaterialImage> green{tile, tile};
    auto blocks = hog::render::Blocks::create(*device, colour_format, depth_format, green);
    REQUIRE(blocks);

    // A green wall twenty metres east of the eye, as above.
    const DVec3 up{0.0, 0.0, 1.0};
    const DVec3 east{1.0, 0.0, 0.0};
    const DVec3 north{0.0, 1.0, 0.0};
    const hog::WorldPosition eye{up * (earth + 10.0)};
    const auto wall = std::make_shared<hog::terrain::BlockMesh>();
    wall->origin = eye;
    for (const auto& [across, high] : std::array<std::pair<double, double>, 4>{
             {{-20.0, -20.0}, {20.0, -20.0}, {20.0, 20.0}, {-20.0, 20.0}}}) {
        wall->vertices.push_back(
            {.position = hog::Vec3{(east * 20.0) + (north * across) + (up * high)},
             .normal = hog::Vec3{-east},
             .tiles = {0, 0, 0},
             .corner = 0});
    }
    wall->indices = {0, 2, 1, 0, 3, 2};
    hog::terrain::mix_corners(*wall);
    blocks->show(wall);

    const hog::render::SkyView view{
        .camera = {.eye = eye, .forward = east, .up = up},
        .sun = glm::normalize(DVec3{0.0, 1.0, 1.0}),
        .bend = {.up = up, .eye_height = 10.0, .radius = earth, .faked = earth},
        .exposure = 1.0};
    const auto record = [&] {
        REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
            CHECK(sky->record_frame(commands, {.width = 64, .height = 64}, 0, *atmosphere, view));
            CHECK(blocks->prepare(0));
            const auto around = [&](VkCommandBuffer drawing, const hog::render::Camera& camera,
                                    VkExtent2D extent) -> hog::Result<> {
                blocks->draw_prepared(drawing, extent,
                                      {.camera = camera, .sun = view.sun, .bend = view.bend}, 0);
                return {};
            };
            CHECK(probe->record(commands, 0, *atmosphere, *sky, {.sky = view, .around = around}));
        }));
    };
    const auto east_is_green = [&] {
        const std::array<float, 4> found = sampled(
            *device, *probe, std::vector<DVec3>{hog::render::probe_direction(east, up, east)})[0];
        CAPTURE(found[0], found[1], found[2]);
        return found[1] > 2.0F * found[0] && found[1] > 2.0F * found[2];
    };

    record();
    REQUIRE(east_is_green());
    // Gone, the wall stays in the east face until its turn comes round: after the four others.
    blocks->show(std::make_shared<hog::terrain::BlockMesh>());
    for (int other = 0; other < 4; ++other) {
        record();
        CHECK(east_is_green());
    }
    record();
    CHECK_FALSE(east_is_green());
    CHECK(device->validation_errors() == 0);
}
