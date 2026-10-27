#include "test_device.hpp"

#include <hog/planet/climate.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/device.hpp>
#include <hog/render/environment_probe.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/terrain.hpp>
#include <hog/shaders/cube_probe.hpp>
#include <hog/terrain/material.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/stitching.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <span>
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

// Grey all over.
std::vector<hog::terrain::MaterialImage> plain_materials() {
    std::vector<hog::terrain::MaterialImage> materials;
    materials.reserve(hog::planet::biome_count);
    for (int biome = 0; biome < hog::planet::biome_count; ++biome) {
        materials.push_back(hog::terrain::material_image({.colour = DVec3{0.5}}, 64));
    }
    return materials;
}

// Level ground facing up along +z, `width` metres square about the patch's origin.
std::vector<hog::terrain::PatchVertex> level_patch(float width) {
    using hog::terrain::patch_cells;
    using hog::terrain::patch_side;
    const float spacing = width / static_cast<float>(patch_cells);
    constexpr int middle = patch_cells / 2;
    std::vector<hog::terrain::PatchVertex> vertices;
    for (int j = 0; j < patch_side; ++j) {
        for (int i = 0; i < patch_side; ++i) {
            vertices.push_back({.position = {static_cast<float>(i - middle) * spacing,
                                             static_cast<float>(j - middle) * spacing, 0.0F},
                                .normal = {0.0F, 0.0F, 1.0F}});
        }
    }
    for (int edge = 0; edge < 4; ++edge) {
        for (int along = 0; along < patch_side; ++along) {
            const hog::terrain::GridPoint above = hog::terrain::on_edge(edge, along);
            vertices.push_back(vertices.at((static_cast<std::size_t>(above.j) * patch_side) +
                                           static_cast<std::size_t>(above.i)));
        }
    }
    return vertices;
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
    auto terrain = hog::render::Terrain::create(*device, colour_format, depth_format,
                                                {.materials = plain_materials()});
    REQUIRE(terrain);
    auto pool = hog::render::TerrainPool::create(*device, {});
    REQUIRE(pool);
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
        probe->record(commands, 0, *atmosphere, *sky, *terrain, *pool,
                      {.sky = view, .terrain = {}, .ground = {}});
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

TEST_CASE("a probe's cube holds the ground the way it lies from the eye", "[render][gpu]") {
    const auto device = test_device();
    auto atmosphere = hog::render::Atmosphere::create(*device);
    REQUIRE(atmosphere);
    auto sky = hog::render::Sky::create(*device, colour_format, depth_format);
    REQUIRE(sky);
    auto terrain = hog::render::Terrain::create(*device, colour_format, depth_format,
                                                {.materials = plain_materials()});
    REQUIRE(terrain);
    auto pool = hog::render::TerrainPool::create(*device, {});
    REQUIRE(pool);
    auto probe = hog::render::EnvironmentProbe::create(*device, colour_format, depth_format, 64);
    REQUIRE(probe);
    const std::uint32_t slot = pool->acquire().value_or(1);
    REQUIRE(slot == 0);
    REQUIRE(pool->stage(0, slot, level_patch(200.0F)));

    // Ground 5 m under the eye, all of it to the north; the sun overhead.
    const DVec3 up{0.0, 0.0, 1.0};
    const DVec3 east{1.0, 0.0, 0.0};
    const hog::WorldPosition eye{up * (earth + 10.0)};
    const hog::render::SkyView view{
        .camera = {.eye = eye, .forward = east, .up = up},
        .sun = up,
        .bend = {.up = up, .eye_height = 10.0, .radius = earth, .faked = earth},
        .exposure = 1.0};
    const std::array ground{hog::render::TerrainDraw{
        .slot = slot, .origin = hog::WorldPosition{(up * (earth + 5.0)) + DVec3{0.0, 120.0, 0.0}}}};
    REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
        REQUIRE(pool->record_uploads(commands, 0));
        CHECK(sky->record_frame(commands, {.width = 64, .height = 64}, 0, *atmosphere, view));
        probe->record(commands, 0, *atmosphere, *sky, *terrain, *pool,
                      {.sky = view,
                       .terrain = {.camera = view.camera, .sun = up, .night_light = 1.0},
                       .ground = ground});
    }));

    const auto below = [&](double x, double y) {
        return glm::normalize((DVec3{x, y, 0.0} * std::cos(7.0 * degree)) -
                              (up * std::sin(7.0 * degree)));
    };
    const std::vector<std::array<float, 4>> found =
        sampled(*device, *probe,
                {hog::render::probe_direction(below(0.0, 1.0), up, east),
                 hog::render::probe_direction(below(0.0, -1.0), up, east),
                 hog::render::probe_direction(below(1.0, 0.0), up, east)});
    const double north = brightness(found[0]);
    const double south = brightness(found[1]);
    CAPTURE(north, south, brightness(found[2]));
    // Grey ground to the north, lit by the sun overhead; south and east, nothing but the sky.
    CHECK(std::abs(static_cast<double>(found[0][0] - found[0][2])) < 0.05);
    CHECK(north > south + 0.5);
    CHECK(std::abs(brightness(found[2]) - south) < 0.1 * north);
    CHECK(device->validation_errors() == 0);
}
