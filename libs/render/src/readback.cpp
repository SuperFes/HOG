#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/readback.hpp>

#include <algorithm>
#include <cstddef>

namespace hog::render {

Result<std::vector<std::uint8_t>> read_pixels(Device& device, const Image& image, ImageUse use) {
    const VkExtent2D extent = image.desc().extent;
    const VkDeviceSize size = VkDeviceSize{4} * extent.width * extent.height;
    auto buffer = make_buffer(device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Readback);
    if (!buffer) {
        return std::unexpected(std::move(buffer).error());
    }
    const auto copied = device.submit_now([&](VkCommandBuffer commands) {
        transition(commands, image.handle(), image.desc().aspect, use, image_use::copy_source);
        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = image.desc().aspect,
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageOffset = {},
            .imageExtent = {.width = extent.width, .height = extent.height, .depth = 1}};
        vkCmdCopyImageToBuffer(commands, image.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffer->handle(), 1, &region);
    });
    if (!copied) {
        return std::unexpected(copied.error());
    }
    if (auto visible = buffer->invalidate(); !visible) {
        return std::unexpected(std::move(visible).error());
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size));
    std::ranges::transform(buffer->mapped(), pixels.begin(),
                           [](std::byte value) { return static_cast<std::uint8_t>(value); });
    return pixels;
}

} // namespace hog::render
