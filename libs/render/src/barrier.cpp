#include <hog/render/barrier.hpp>

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

void record(VkCommandBuffer commands, const VkImageMemoryBarrier2& barrier) noexcept {
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

} // namespace

void transition(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, ImageUse from,
                ImageUse to) noexcept {
    record(commands, image_barrier(image, aspect, from, to));
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
        record(commands, *barrier);
    }
}

} // namespace hog::render
