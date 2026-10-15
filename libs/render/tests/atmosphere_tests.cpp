#include "test_device.hpp"

#include <hog/atmosphere/model.hpp>
#include <hog/render/atmosphere.hpp>
#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/shaders/air_probe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using hog::DVec3;
using hog::atmosphere::Model;
using hog::render::Atmosphere;
using hog::render::testing::test_device;

namespace {

using Float4 = std::array<float, 4>;

struct Found {
    DVec3 transmittance;
    DVec3 scattering;
};

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type) {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr};
}

// Reads each table on the GPU: the transmittance at a height and direction, and multiple scattering
// at a height and the sun's direction.
std::vector<Found> read_tables(hog::render::Device& device, const Atmosphere& atmosphere,
                               std::span<const Float4> asked) {
    constexpr std::array bindings{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                  binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                  binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                  binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                                  binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
    auto pipeline = hog::render::make_compute_pipeline(
        device, {.spirv = hog::shaders::air_probe, .descriptors = bindings});
    REQUIRE(pipeline);
    auto questions = hog::render::make_filled_buffer(device, std::as_bytes(asked),
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(questions);
    auto answers =
        hog::render::make_buffer(device, asked.size_bytes() * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 hog::render::Memory::Readback);
    REQUIRE(answers);
    const auto run = device.submit_now([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->handle());
        hog::render::push_uniform_buffer(commands, *pipeline, 0, atmosphere.constants());
        hog::render::push_sampled_image(commands, *pipeline, 1, atmosphere.transmittance().view(),
                                        atmosphere.sampler());
        hog::render::push_sampled_image(commands, *pipeline, 2, atmosphere.scattering().view(),
                                        atmosphere.sampler());
        hog::render::push_storage_buffer(commands, *pipeline, 3, questions->handle());
        hog::render::push_storage_buffer(commands, *pipeline, 4, answers->handle());
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
    });
    REQUIRE(run);
    REQUIRE(answers->invalidate());
    std::vector<Float4> raw(asked.size() * 2);
    std::memcpy(raw.data(), answers->mapped().data(), raw.size() * sizeof(Float4));
    std::vector<Found> found;
    found.reserve(asked.size());
    for (std::size_t at = 0; at < asked.size(); ++at) {
        const Float4& transmittance = raw[2 * at];
        const Float4& scattering = raw[(2 * at) + 1];
        found.push_back({.transmittance = {transmittance[0], transmittance[1], transmittance[2]},
                         .scattering = {scattering[0], scattering[1], scattering[2]}});
    }
    return found;
}

// Within half-float precision and float rounding of the GPU, on top of `slack` of the larger.
bool close(DVec3 gpu, DVec3 cpu, double slack) {
    for (glm::length_t channel = 0; channel < 3; ++channel) {
        const double allowed =
            2e-4 + (slack * std::max(std::abs(gpu[channel]), std::abs(cpu[channel])));
        if (std::abs(gpu[channel] - cpu[channel]) > allowed) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("the air's tables on the GPU match the CPU model's", "[render][gpu][atmosphere]") {
    const auto device = test_device();
    auto atmosphere = Atmosphere::create(*device);
    REQUIRE(atmosphere);
    const Model model;

    std::vector<Float4> asked;
    const std::array heights{0.0F, 2.0F, 400.0F, 3'000.0F, 20'000.0F, 70'000.0F};
    const std::array directions{1.0F, 0.6F, 0.2F, 0.05F, 0.01F, 0.0F, -0.02F, -0.5F};
    const std::array suns{1.0F, 0.3F, 0.05F, 0.0F, -0.1F, -0.4F};
    for (const float height : heights) {
        for (std::size_t at = 0; at < directions.size(); ++at) {
            asked.push_back({height, directions.at(at), height, suns.at(at % suns.size())});
        }
    }
    const std::vector<Found> found = read_tables(*device, *atmosphere, asked);
    REQUIRE(found.size() == asked.size());
    for (std::size_t at = 0; at < asked.size(); ++at) {
        const Float4& question = asked[at];
        const DVec3 transmittance =
            model.transmittance(static_cast<double>(question[0]), static_cast<double>(question[1]));
        const DVec3 scattering = model.multiple_scattering(static_cast<double>(question[2]),
                                                           static_cast<double>(question[3]));
        INFO("height " << question[0] << ", mu " << question[1] << ", sun " << question[3]);
        INFO("transmittance " << found[at].transmittance.r << ", " << found[at].transmittance.g
                              << ", " << found[at].transmittance.b << " against " << transmittance.r
                              << ", " << transmittance.g << ", " << transmittance.b);
        CHECK(close(found[at].transmittance, transmittance, 5e-3));
        INFO("scattering " << found[at].scattering.r << ", " << found[at].scattering.g << ", "
                           << found[at].scattering.b << " against " << scattering.r << ", "
                           << scattering.g << ", " << scattering.b);
        CHECK(close(found[at].scattering, scattering, 1e-2));
    }
    CHECK(device->validation_errors() == 0);
}
