#pragma once

#include <hog/render/vulkan.hpp>

#include <optional>

namespace hog::render {

// How an image was last used, or is about to be: what a barrier waits on and makes ready.
struct ImageUse {
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2 access = VK_ACCESS_2_NONE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

    friend bool operator==(const ImageUse&, const ImageUse&) = default;
};

namespace image_use {

// Contents not needed.
inline constexpr ImageUse discard{};
inline constexpr ImageUse colour_target{.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                        .access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                                                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
inline constexpr ImageUse depth_target{.stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                       .access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                       .layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL};
// By either shader stage.
inline constexpr ImageUse sampled{.stage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                  .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
inline constexpr ImageUse copy_source{.stage = VK_PIPELINE_STAGE_2_COPY_BIT,
                                      .access = VK_ACCESS_2_TRANSFER_READ_BIT,
                                      .layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
inline constexpr ImageUse copy_target{.stage = VK_PIPELINE_STAGE_2_COPY_BIT,
                                      .access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                      .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
inline constexpr ImageUse present{.stage = VK_PIPELINE_STAGE_2_NONE,
                                  .access = VK_ACCESS_2_NONE,
                                  .layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};

} // namespace image_use

// The barrier between two uses of the whole of an image.
[[nodiscard]] VkImageMemoryBarrier2 image_barrier(VkImage image, VkImageAspectFlags aspect,
                                                  ImageUse from, ImageUse to) noexcept;

// Records the barrier between two uses of an image.
void transition(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, ImageUse from,
                ImageUse to) noexcept;

// Whether moving between two uses needs a barrier: a layout change, or a write on either side.
[[nodiscard]] bool needs_barrier(ImageUse from, ImageUse to) noexcept;

// An image and how it was last used, so each new use waits on exactly the one before.
class TrackedImage {
public:
    TrackedImage() = default;
    TrackedImage(VkImage image, VkImageAspectFlags aspect, ImageUse use = image_use::discard)
        : image_{image}, aspect_{aspect}, use_{use} {}

    [[nodiscard]] VkImage handle() const noexcept { return image_; }
    [[nodiscard]] ImageUse use() const noexcept { return use_; }

    // The barrier into `next`, where one is needed; either way the image is in `next` after.
    [[nodiscard]] std::optional<VkImageMemoryBarrier2> barrier_to(ImageUse next) noexcept;
    // Records the barrier into `next`, where one is needed.
    void use_as(VkCommandBuffer commands, ImageUse next) noexcept;

private:
    VkImage image_ = VK_NULL_HANDLE;
    VkImageAspectFlags aspect_ = VK_IMAGE_ASPECT_COLOR_BIT;
    ImageUse use_ = image_use::discard;
};

} // namespace hog::render
