#pragma once

#include <hog/render/vulkan.hpp>

#include <optional>

namespace hog::render {

// Begins dynamic rendering into one colour image, which must already be a colour target, and
// sets the viewport and scissor to the whole of it. Without `clear` its contents are kept.
void begin_rendering(VkCommandBuffer commands, VkImageView view, VkExtent2D extent,
                     std::optional<VkClearColorValue> clear) noexcept;

// A depth image to test and write against, which must already be a depth target.
struct DepthAttachment {
    VkImageView view = VK_NULL_HANDLE;
    // Depth is reversed, so 0 is as far as can be. Without it, the contents are kept.
    std::optional<float> clear = 0.0F;
};

// As above, with a depth image the same size as the colour one.
void begin_rendering(VkCommandBuffer commands, VkImageView view, VkExtent2D extent,
                     std::optional<VkClearColorValue> clear, const DepthAttachment& depth) noexcept;

} // namespace hog::render
