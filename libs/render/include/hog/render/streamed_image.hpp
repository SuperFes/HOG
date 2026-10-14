#pragma once

#include <hog/core/result.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::render {

class Device;

// Bytes per texel of the formats an image can be streamed in, or empty for any other.
[[nodiscard]] std::optional<std::uint32_t> texel_bytes(VkFormat format) noexcept;

// A sampled image the CPU replaces while frames are in flight. Each frame slot stages through its
// own buffer, so an upload never overwrites one the GPU is still copying from.
class StreamedImage {
public:
    [[nodiscard]] const Image& image() const noexcept { return image_; }
    // Whether it's been written yet; until then it mustn't be sampled.
    [[nodiscard]] bool ready() const noexcept { return tracked_.use() == image_use::sampled; }

    // Records copying in `texels`, rows packed tightly from the top and layers one after another,
    // and readies the image for fragment shaders. The slot's last frame must have finished.
    Result<> update(VkCommandBuffer commands, std::uint32_t slot,
                    std::span<const std::byte> texels);

private:
    friend Result<StreamedImage> make_streamed_image(const Device& device, VkFormat format,
                                                     VkExtent2D extent, std::uint32_t slots,
                                                     std::uint32_t layers);

    Image image_;
    TrackedImage tracked_;
    std::vector<Buffer> staging_;
};

[[nodiscard]] Result<StreamedImage> make_streamed_image(const Device& device, VkFormat format,
                                                        VkExtent2D extent, std::uint32_t slots,
                                                        std::uint32_t layers = 1);

} // namespace hog::render
