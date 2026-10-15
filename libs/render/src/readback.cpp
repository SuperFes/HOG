#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace hog::render {

namespace {

std::uint32_t pixel_bytes_of(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 4;
    }
}

} // namespace

Result<Buffer> make_readback(const Device& device, VkExtent2D extent, std::uint32_t pixel_bytes) {
    const VkDeviceSize size = VkDeviceSize{pixel_bytes} * extent.width * extent.height;
    return make_buffer(device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Readback);
}

void record_readback(VkCommandBuffer commands, TrackedImage& image, VkExtent2D extent,
                     const Buffer& buffer) noexcept {
    image.use_as(commands, image_use::copy_source);
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
        .imageOffset = {},
        .imageExtent = {.width = extent.width, .height = extent.height, .depth = 1}};
    vkCmdCopyImageToBuffer(commands, image.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffer.handle(), 1, &region);
}

Result<std::vector<std::uint8_t>> read_back(const Buffer& buffer) {
    if (auto visible = buffer.invalidate(); !visible) {
        return std::unexpected(std::move(visible).error());
    }
    std::vector<std::uint8_t> pixels(buffer.mapped().size());
    std::ranges::transform(buffer.mapped(), pixels.begin(),
                           [](std::byte value) { return static_cast<std::uint8_t>(value); });
    return pixels;
}

Result<std::vector<std::uint8_t>> read_pixels(Device& device, const Image& image, ImageUse use) {
    const VkExtent2D extent = image.desc().extent;
    auto buffer = make_readback(device, extent, pixel_bytes_of(image.desc().format));
    if (!buffer) {
        return std::unexpected(std::move(buffer).error());
    }
    TrackedImage tracked{image.handle(), image.desc().aspect, use};
    if (auto copied = device.submit_now(
            [&](VkCommandBuffer commands) { record_readback(commands, tracked, extent, *buffer); });
        !copied) {
        return std::unexpected(std::move(copied).error());
    }
    return read_back(*buffer);
}

Result<std::vector<std::uint8_t>> to_rgb(std::span<const std::uint8_t> pixels, VkFormat format) {
    bool swapped = false;
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        break;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        swapped = true;
        break;
    default:
        return make_error(ErrorCode::Unsupported, "can't read colours from that format");
    }
    std::vector<std::uint8_t> rgb;
    rgb.reserve(pixels.size() / 4 * 3);
    for (std::size_t at = 0; at + 4 <= pixels.size(); at += 4) {
        rgb.push_back(pixels[swapped ? at + 2 : at]);
        rgb.push_back(pixels[at + 1]);
        rgb.push_back(pixels[swapped ? at : at + 2]);
    }
    return rgb;
}

} // namespace hog::render
