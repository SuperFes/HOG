#include <hog/render/barrier.hpp>

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

void transition(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, ImageUse from,
                ImageUse to) noexcept {
    const VkImageMemoryBarrier2 barrier = image_barrier(image, aspect, from, to);
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .pNext = nullptr,
                                      .dependencyFlags = 0,
                                      .memoryBarrierCount = 0,
                                      .pMemoryBarriers = nullptr,
                                      .bufferMemoryBarrierCount = 0,
                                      .pBufferMemoryBarriers = nullptr,
                                      .imageMemoryBarrierCount = 1,
                                      .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(commands, &dependency);
}

} // namespace hog::render
