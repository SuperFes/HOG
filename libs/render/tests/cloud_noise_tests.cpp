#include "test_device.hpp"

#include <hog/render/cloud_noise.hpp>
#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/shaders/volume_probe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

using hog::DVec3;
using hog::render::detail_noise;
using hog::render::shape_noise;
using hog::render::tiled_cells;
using hog::render::tiled_gradient;
using hog::render::testing::test_device;

namespace {

constexpr std::uint32_t size = 16;

int texel(const std::vector<std::byte>& volume, std::uint32_t x, std::uint32_t y, std::uint32_t z,
          std::uint32_t channel) {
    return std::to_integer<int>(
        volume.at((((((std::size_t{z} * size) + y) * size) + x) * 4) + channel));
}

// Of each channel, the mean step between neighbours along x: across the volume's edge, and within
// it.
struct Steps {
    std::array<double, 4> across{};
    std::array<double, 4> within{};
};

Steps steps(const std::vector<std::byte>& volume) {
    Steps out;
    for (std::uint32_t c = 0; c < 4; ++c) {
        double across = 0.0;
        double within = 0.0;
        for (std::uint32_t z = 0; z < size; ++z) {
            for (std::uint32_t y = 0; y < size; ++y) {
                across += std::abs(texel(volume, size - 1, y, z, c) - texel(volume, 0, y, z, c));
                for (std::uint32_t x = 0; x + 1 < size; ++x) {
                    within += std::abs(texel(volume, x, y, z, c) - texel(volume, x + 1, y, z, c));
                }
            }
        }
        out.across.at(c) = across / (size * size);
        out.within.at(c) = within / (size * size * (size - 1));
    }
    return out;
}

} // namespace

TEST_CASE("cells and gradient noise repeat every 1 and keep to their ranges", "[render]") {
    double most = 0.0;
    double least = 1.0;
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < 400; ++i) {
        const DVec3 at{std::fmod(i * 0.1373, 1.0), std::fmod(i * 0.2911, 1.0),
                       std::fmod(i * 0.0577, 1.0)};
        for (const DVec3 by : {DVec3{1.0, 0.0, 0.0}, DVec3{0.0, -1.0, 0.0}, DVec3{0.0, 0.0, 3.0}}) {
            CHECK(std::abs(tiled_cells(at, 5, 9) - tiled_cells(at + by, 5, 9)) < 1e-9);
            CHECK(std::abs(tiled_gradient(at, 6, 9) - tiled_gradient(at + by, 6, 9)) < 1e-9);
        }
        const double cells = tiled_cells(at, 5, 9);
        CHECK(cells >= 0.0);
        CHECK(cells <= 1.0);
        most = std::max(most, cells);
        least = std::min(least, cells);
        const double gradient = tiled_gradient(at, 6, 9);
        CHECK(std::abs(gradient) <= 1.1);
        sum += gradient;
        ++count;
    }
    CHECK(most > 0.8);
    CHECK(least < 0.5);
    CHECK(std::abs(sum / count) < 0.1);
    // Nothing at the lattice's corners.
    CHECK(std::abs(tiled_gradient({0.5, 0.25, 0.0}, 4, 9)) < 1e-12);
}

TEST_CASE("the clouds' noise repeats across its edges and comes out the same each time",
          "[render]") {
    for (const auto& volume : {shape_noise(size, 1), detail_noise(size, 1)}) {
        REQUIRE(volume.size() == std::size_t{size} * size * size * 4);
        const Steps found = steps(volume);
        for (std::uint32_t c = 0; c < 4; ++c) {
            CAPTURE(c, found.across.at(c), found.within.at(c));
            CHECK(found.across.at(c) < (2.0 * found.within.at(c)) + 1.0);
            int least = 255;
            int most = 0;
            for (std::size_t at = c; at < volume.size(); at += 4) {
                least = std::min(least, std::to_integer<int>(volume[at]));
                most = std::max(most, std::to_integer<int>(volume[at]));
            }
            CHECK(most - least > 80);
        }
    }
    CHECK(shape_noise(size, 1) == shape_noise(size, 1));
    CHECK(detail_noise(size, 1) == detail_noise(size, 1));
}

TEST_CASE("the billows are spread evenly, so cover covers its share", "[render]") {
    const std::vector<std::byte> volume = shape_noise(size, 1);
    std::array<std::size_t, 4> quarters{};
    for (std::size_t at = 0; at < volume.size(); at += 4) {
        ++quarters.at(std::to_integer<std::size_t>(volume[at]) / 64);
    }
    for (const std::size_t quarter : quarters) {
        CHECK(std::abs(static_cast<double>(quarter) - (size * size * size / 4.0)) <
              0.05 * size * size * size);
    }
}

TEST_CASE("each mip of the clouds' noise is the mean of the one before", "[render]") {
    const std::vector<std::byte> volume = detail_noise(size, 3);
    const std::size_t first = std::size_t{size} * size * size * 4;
    const std::size_t second = first / 8;
    REQUIRE(volume.size() == first + second + (second / 8));
    for (std::uint32_t c = 0; c < 4; ++c) {
        int sum = 0;
        for (std::uint32_t corner = 0; corner < 8; ++corner) {
            sum +=
                texel(volume, 2 + (corner & 1U), 4 + ((corner >> 1U) & 1U), 6 + (corner >> 2U), c);
        }
        const std::size_t half = size / 2;
        const std::size_t at = first + (((((3 * half) + 2) * half) + 1) * 4) + c;
        CHECK(std::abs(std::to_integer<int>(volume.at(at)) - (sum / 8)) <= 1);
    }
}

TEST_CASE("the clouds' noise is sampled on the GPU as baked, repeating", "[render][gpu]") {
    const auto device = test_device();
    hog::jobs::JobSystem jobs{2};
    auto noise = hog::render::make_cloud_noise(*device, jobs);
    REQUIRE(noise);
    auto sampler = hog::render::make_tiling_sampler(*device);
    REQUIRE(sampler);
    const std::vector<std::byte> baked =
        detail_noise(hog::render::detail_noise_size, hog::render::noise_levels, &jobs);
    const float texel_width = 1.0F / static_cast<float>(hog::render::detail_noise_size);
    // A texel's middle, and the same across each edge.
    const std::vector<std::array<float, 4>> asked{
        {(3.5F * texel_width), (5.5F * texel_width), (7.5F * texel_width), 0.0F},
        {0.0F, 0.3F, 0.6F, 0.0F},
        {1.0F, 0.3F, 0.6F, 0.0F},
        {0.2F, 0.0F, 0.6F, 0.0F},
        {0.2F, 1.0F, 0.6F, 0.0F}};
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
    auto probe = hog::render::make_compute_pipeline(
        *device, {.spirv = hog::shaders::volume_probe, .descriptors = bindings});
    REQUIRE(probe);
    auto questions = hog::render::make_filled_buffer(*device, std::as_bytes(std::span{asked}),
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(questions);
    auto answers =
        hog::render::make_buffer(*device, std::span{asked}.size_bytes(),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(answers);
    REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe->handle());
        hog::render::push_sampled_image(commands, *probe, 0, noise->detail.view(),
                                        sampler->handle());
        hog::render::push_storage_buffer(commands, *probe, 1, questions->handle());
        hog::render::push_storage_buffer(commands, *probe, 2, answers->handle());
        vkCmdDispatch(commands, 1, 1, 1);
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

    constexpr std::size_t side = hog::render::detail_noise_size;
    const std::size_t middle = ((((std::size_t{7} * side) + 5) * side) + 3) * 4;
    for (std::size_t c = 0; c < 4; ++c) {
        CAPTURE(c);
        CHECK(std::abs((found[0].at(c) * 255.0F) -
                       static_cast<float>(std::to_integer<int>(baked.at(middle + c)))) < 1.0F);
        CHECK(std::abs(found[1].at(c) - found[2].at(c)) < 1e-3F);
        CHECK(std::abs(found[3].at(c) - found[4].at(c)) < 1e-3F);
    }
    // A 3D image is one layer deep in slices.
    CHECK_FALSE(hog::render::make_image(*device, {.extent = {.width = 4, .height = 4},
                                                  .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                                                  .layers = 2,
                                                  .depth = 4}));
    CHECK(device->validation_errors() == 0);
}
