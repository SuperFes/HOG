#include <hog/render/rendering.hpp>

namespace hog::render {

namespace {

void begin(VkCommandBuffer commands, VkImageView view, VkExtent2D extent,
           std::optional<VkClearColorValue> clear, const DepthAttachment* depth) noexcept {
    const VkRenderingAttachmentInfo colour{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = clear.value_or(VkClearColorValue{})}};
    const VkRenderingAttachmentInfo depth_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = depth != nullptr ? depth->view : VK_NULL_HANDLE,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = depth != nullptr && depth->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                   : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {
            .depthStencil = {.depth = depth != nullptr ? depth->clear.value_or(0.0F) : 0.0F,
                             .stencil = 0}}};
    const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                    .pNext = nullptr,
                                    .flags = 0,
                                    .renderArea = {.offset = {}, .extent = extent},
                                    .layerCount = 1,
                                    .viewMask = 0,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &colour,
                                    .pDepthAttachment = depth != nullptr ? &depth_info : nullptr,
                                    .pStencilAttachment = nullptr};
    vkCmdBeginRendering(commands, &rendering);
    const VkViewport viewport{.x = 0.0F,
                              .y = 0.0F,
                              .width = static_cast<float>(extent.width),
                              .height = static_cast<float>(extent.height),
                              .minDepth = 0.0F,
                              .maxDepth = 1.0F};
    const VkRect2D scissor{.offset = {}, .extent = extent};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &scissor);
}

} // namespace

void begin_rendering(VkCommandBuffer commands, VkImageView view, VkExtent2D extent,
                     std::optional<VkClearColorValue> clear) noexcept {
    begin(commands, view, extent, clear, nullptr);
}

void begin_rendering(VkCommandBuffer commands, VkImageView view, VkExtent2D extent,
                     std::optional<VkClearColorValue> clear,
                     const DepthAttachment& depth) noexcept {
    begin(commands, view, extent, clear, &depth);
}

} // namespace hog::render
