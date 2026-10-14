#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/readback.hpp>
#include <hog/shaders/triangle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

using hog::render::testing::test_device;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

Pixel pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t width, std::uint32_t x,
               std::uint32_t y) {
    const std::size_t first = 4 * ((std::size_t{y} * width) + x);
    return {pixels.at(first), pixels.at(first + 1), pixels.at(first + 2), pixels.at(first + 3)};
}

} // namespace

TEST_CASE("a headless device draws a triangle and reads it back", "[render][gpu]") {
    const auto device = test_device();
    constexpr VkExtent2D extent{.width = 32, .height = 32};
    auto target = hog::render::make_image(
        *device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                  .extent = extent,
                  .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(target);
    auto pipeline = hog::render::make_pipeline(*device, {.spirv = hog::shaders::triangle});
    REQUIRE(pipeline);

    const auto drawn = device->submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        const VkRenderingAttachmentInfo colour{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .pNext = nullptr,
            .imageView = target->view(),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .resolveMode = VK_RESOLVE_MODE_NONE,
            .resolveImageView = VK_NULL_HANDLE,
            .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = {.color = {.float32 = {0.0F, 0.0F, 1.0F, 1.0F}}}};
        const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                        .pNext = nullptr,
                                        .flags = 0,
                                        .renderArea = {.offset = {}, .extent = extent},
                                        .layerCount = 1,
                                        .viewMask = 0,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachments = &colour,
                                        .pDepthAttachment = nullptr,
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
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRendering(commands);
    });
    REQUIRE(drawn);

    const auto pixels =
        hog::render::read_pixels(*device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    REQUIRE(pixels->size() == std::size_t{4} * extent.width * extent.height);
    CHECK(pixel_at(*pixels, extent.width, 16, 16) == Pixel{255, 0, 0, 255});
    CHECK(pixel_at(*pixels, extent.width, 1, 1) == Pixel{0, 0, 255, 255});
    CHECK(pixel_at(*pixels, extent.width, 30, 30) == Pixel{0, 0, 255, 255});
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("buffers carry bytes to the GPU and back", "[render][gpu]") {
    const auto device = test_device();
    constexpr VkDeviceSize size = 256;
    auto upload = hog::render::make_buffer(*device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           hog::render::Memory::Upload);
    auto readback = hog::render::make_buffer(*device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             hog::render::Memory::Readback);
    REQUIRE(upload);
    REQUIRE(readback);
    REQUIRE(upload->mapped().size() == size);
    for (std::size_t at = 0; at < size; ++at) {
        upload->mapped()[at] = static_cast<std::byte>(at * 7);
    }

    hog::render::Buffer moved = std::move(*upload);
    CHECK(upload->handle() == VK_NULL_HANDLE);
    REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
        const VkBufferCopy region{.srcOffset = 0, .dstOffset = 0, .size = size};
        vkCmdCopyBuffer(commands, moved.handle(), readback->handle(), 1, &region);
    }));
    REQUIRE(readback->invalidate());
    CHECK(std::ranges::equal(moved.mapped(), readback->mapped()));
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("Vulkan errors name the call and its result", "[render]") {
    const auto unsupported = hog::render::vulkan_error(VK_ERROR_INCOMPATIBLE_DRIVER, "loading");
    CHECK(unsupported.error().code == hog::ErrorCode::Unsupported);
    CHECK(unsupported.error().message == "loading: VK_ERROR_INCOMPATIBLE_DRIVER");
    const auto lost = hog::render::vulkan_error(VK_ERROR_DEVICE_LOST, "drawing");
    CHECK(lost.error().code == hog::ErrorCode::Internal);
    CHECK(lost.error().message == "drawing: VK_ERROR_DEVICE_LOST");
}
