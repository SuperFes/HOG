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
    // and readies the image for shaders to sample. The slot's last frame must have finished.
    Result<> update(VkCommandBuffer commands, std::uint32_t slot,
                    std::span<const std::byte> texels);

private:
    friend Result<StreamedImage> make_streamed_image(const Device& device, VkFormat format,
                                                     VkExtent2D extent, std::uint32_t slots,
                                                     std::uint32_t layers, bool cube);

    Image image_;
    TrackedImage tracked_;
    std::vector<Buffer> staging_;
};

// A cube takes six square layers, the faces in Vulkan's order.
[[nodiscard]] Result<StreamedImage> make_streamed_image(const Device& device, VkFormat format,
                                                        VkExtent2D extent, std::uint32_t slots,
                                                        std::uint32_t layers = 1,
                                                        bool cube = false);

// A sampled image filled once from `texels`: each mip in turn from the largest, its layers or
// slices one after another, rows packed tightly from the top. It's ready to sample once this
// returns.
[[nodiscard]] Result<Image> make_filled_image(Device& device, const ImageDesc& desc,
                                              std::span<const std::byte> texels);

} // namespace hog::render
