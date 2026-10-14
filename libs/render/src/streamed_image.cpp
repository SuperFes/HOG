#include <hog/render/device.hpp>
#include <hog/render/streamed_image.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace hog::render {

std::optional<std::uint32_t> texel_bytes(VkFormat format) noexcept {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R32_SFLOAT:
        return 4;
    default:
        return std::nullopt;
    }
}

Result<> StreamedImage::update(VkCommandBuffer commands, std::uint32_t slot,
                               std::span<const std::byte> texels) {
    if (slot >= staging_.size()) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("no frame slot {} of {}", slot, staging_.size()));
    }
    const Buffer& staging = staging_[slot];
    if (texels.size() != staging.mapped().size()) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("{} bytes for an image of {}", texels.size(), staging.mapped().size()));
    }
    std::ranges::copy(texels, staging.mapped().begin());
    if (auto flushed = staging.flush(); !flushed) {
        return flushed;
    }
    tracked_.use_as(commands, image_use::copy_target);
    const VkExtent2D extent = image_.desc().extent;
    const VkBufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = image_.desc().layers},
        .imageOffset = {},
        .imageExtent = {.width = extent.width, .height = extent.height, .depth = 1}};
    vkCmdCopyBufferToImage(commands, staging.handle(), image_.handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    tracked_.use_as(commands, image_use::sampled);
    return {};
}

Result<StreamedImage> make_streamed_image(const Device& device, VkFormat format, VkExtent2D extent,
                                          std::uint32_t slots, std::uint32_t layers) {
    const std::optional<std::uint32_t> bytes = texel_bytes(format);
    if (!bytes) {
        return make_error(ErrorCode::Unsupported, "images can't be streamed in that format");
    }
    if (extent.width == 0 || extent.height == 0 || slots == 0 || layers == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "a streamed image needs area, a layer and a slot");
    }
    // Readable too, so tests can check what arrived.
    auto image =
        make_image(device, {.format = format,
                            .extent = extent,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .layers = layers});
    if (!image) {
        return std::unexpected(std::move(image).error());
    }
    StreamedImage streamed;
    streamed.image_ = std::move(image).value();
    streamed.tracked_ = TrackedImage{streamed.image_.handle(), VK_IMAGE_ASPECT_COLOR_BIT};
    const VkDeviceSize size = VkDeviceSize{*bytes} * extent.width * extent.height * layers;
    for (std::uint32_t slot = 0; slot < slots; ++slot) {
        auto staging = make_buffer(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload);
        if (!staging) {
            return std::unexpected(std::move(staging).error());
        }
        streamed.staging_.push_back(std::move(staging).value());
    }
    return streamed;
}

} // namespace hog::render
