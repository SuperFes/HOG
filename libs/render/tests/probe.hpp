#pragma once

#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace hog::render::testing {

using Probed = std::array<float, 4>;

// What the compute shader `spirv` finds, `count` of them, from what it's asked: it reads what's
// asked from storage buffer 0 and writes what it finds to 1, 64 to a group.
inline std::vector<Probed> probed(Device& device, std::span<const std::uint32_t> spirv,
                                  std::span<const Probed> asked, std::size_t count) {
    constexpr auto binding = [](std::uint32_t at) {
        return VkDescriptorSetLayoutBinding{.binding = at,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = nullptr};
    };
    const std::array bindings{binding(0), binding(1)};
    auto probe = make_compute_pipeline(device, {.spirv = spirv, .descriptors = bindings});
    REQUIRE(probe);
    auto questions =
        make_filled_buffer(device, std::as_bytes(asked), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    REQUIRE(questions);
    std::vector<Probed> found(count);
    auto answers = make_buffer(device, std::span{found}.size_bytes(),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Memory::Readback);
    REQUIRE(answers);
    const auto run = device.submit_now([&](VkCommandBuffer commands) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, probe->handle());
        push_storage_buffer(commands, *probe, 0, questions->handle());
        push_storage_buffer(commands, *probe, 1, answers->handle());
        vkCmdDispatch(commands, static_cast<std::uint32_t>((count + 63) / 64), 1, 1);
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
    std::memcpy(found.data(), answers->mapped().data(), std::span{found}.size_bytes());
    return found;
}

} // namespace hog::render::testing
