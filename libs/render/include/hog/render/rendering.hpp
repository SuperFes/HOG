#pragma once

#include <hog/render/vulkan.hpp>

#include <optional>

namespace hog::render {

// Begins dynamic rendering into one colour image, which must already be a colour target, and
// sets the viewport and scissor to the whole of it. Without `clear` its contents are kept.
void begin_rendering(VkCommandBuffer commands, VkImageView view, VkExtent2D extent,
                     std::optional<VkClearColorValue> clear) noexcept;

} // namespace hog::render
