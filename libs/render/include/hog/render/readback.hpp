#pragma once

#include <hog/core/result.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/vulkan.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::render {

class Buffer;
class Device;
class Image;

// Somewhere to copy a colour image of `extent` to, of 4 bytes a pixel unless given.
[[nodiscard]] Result<Buffer> make_readback(const Device& device, VkExtent2D extent,
                                           std::uint32_t pixel_bytes = 4);
// Records copying a colour image into a readback buffer, leaving it a copy source.
void record_readback(VkCommandBuffer commands, TrackedImage& image, VkExtent2D extent,
                     const Buffer& buffer) noexcept;
// What a readback buffer holds, once the GPU has finished copying into it.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_back(const Buffer& buffer);

// Pixels of a colour image of 8-bit, 16-bit float or 32-bit float channels, row by row from the
// top, for tests and captures. The image is left as a copy source.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_pixels(Device& device, const Image& image,
                                                            ImageUse use);

// Red, green and blue of each pixel read back from an image in `format`, without alpha.
[[nodiscard]] Result<std::vector<std::uint8_t>> to_rgb(std::span<const std::uint8_t> pixels,
                                                       VkFormat format);

} // namespace hog::render
