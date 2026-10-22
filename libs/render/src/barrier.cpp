#include <hog/render/barrier.hpp>

#include <cstdint>
#include <utility>

namespace hog::render {

VkImageMemoryBarrier2 image_barrier(VkImage image, VkImageAspectFlags aspect, ImageUse from,
                                    ImageUse to) noexcept {
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = from.stage,
            .srcAccessMask = from.access,
            .dstStageMask = to.stage,
            .dstAccessMask = to.access,
            .oldLayout = from.layout,
            .newLayout = to.layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {.aspectMask = aspect,
                                 .baseMipLevel = 0,
                                 .levelCount = VK_REMAINING_MIP_LEVELS,
                                 .baseArrayLayer = 0,
                                 .layerCount = VK_REMAINING_ARRAY_LAYERS}};
}

namespace {

constexpr VkAccessFlags2 writes = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                  VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;

} // namespace

void record_barriers(VkCommandBuffer commands,
                     std::span<const VkImageMemoryBarrier2> barriers) noexcept {
    if (barriers.empty()) {
        return;
    }
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .pNext = nullptr,
                                      .dependencyFlags = 0,
                                      .memoryBarrierCount = 0,
                                      .pMemoryBarriers = nullptr,
                                      .bufferMemoryBarrierCount = 0,
                                      .pBufferMemoryBarriers = nullptr,
                                      .imageMemoryBarrierCount =
                                          static_cast<std::uint32_t>(barriers.size()),
                                      .pImageMemoryBarriers = barriers.data()};
    vkCmdPipelineBarrier2(commands, &dependency);
}

void transition(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, ImageUse from,
                ImageUse to) noexcept {
    const VkImageMemoryBarrier2 barrier = image_barrier(image, aspect, from, to);
    record_barriers(commands, std::span{&barrier, 1});
}

bool needs_barrier(ImageUse from, ImageUse to) noexcept {
    return from.layout != to.layout || ((from.access | to.access) & writes) != 0;
}

std::optional<VkImageMemoryBarrier2> TrackedImage::barrier_to(ImageUse next) noexcept {
    const ImageUse from = std::exchange(use_, next);
    if (!needs_barrier(from, next)) {
        return std::nullopt;
    }
    return image_barrier(image_, aspect_, from, next);
}

void TrackedImage::use_as(VkCommandBuffer commands, ImageUse next) noexcept {
    if (const auto barrier = barrier_to(next)) {
        record_barriers(commands, std::span{&*barrier, 1});
    }
}

void memory_barrier(VkCommandBuffer commands, VkPipelineStageFlags2 from_stage,
                    VkAccessFlags2 from_access, VkPipelineStageFlags2 to_stage,
                    VkAccessFlags2 to_access) noexcept {
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .pNext = nullptr,
                                   .srcStageMask = from_stage,
                                   .srcAccessMask = from_access,
                                   .dstStageMask = to_stage,
                                   .dstAccessMask = to_access};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .pNext = nullptr,
                                      .dependencyFlags = 0,
                                      .memoryBarrierCount = 1,
                                      .pMemoryBarriers = &barrier,
                                      .bufferMemoryBarrierCount = 0,
                                      .pBufferMemoryBarriers = nullptr,
                                      .imageMemoryBarrierCount = 0,
                                      .pImageMemoryBarriers = nullptr};
    vkCmdPipelineBarrier2(commands, &dependency);
}

} // namespace hog::render
