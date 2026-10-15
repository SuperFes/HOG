#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/shaders/layer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using hog::render::make_streamed_image;
using hog::render::testing::test_device;

namespace {

std::vector<std::byte> filled(std::size_t size, std::uint8_t first) {
    std::vector<std::byte> texels(size);
    for (std::size_t at = 0; at < size; ++at) {
        texels[at] = static_cast<std::byte>(first + at);
    }
    return texels;
}

// Draws one layer of `image` over the whole of a target, returning the target's pixels.
std::vector<std::uint8_t> draw_layer(hog::render::Device& device, const hog::render::Image& image,
                                     float layer) {
    constexpr VkExtent2D extent{.width = 4, .height = 4};
    auto target = hog::render::make_image(
        device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(target);
    const std::array descriptors{
        VkDescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .descriptorCount = 1,
                                     .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                     .pImmutableSamplers = nullptr}};
    auto pipeline = hog::render::make_pipeline(device, {.spirv = hog::shaders::layer,
                                                        .push_constant_bytes = sizeof(float),
                                                        .descriptors = descriptors});
    REQUIRE(pipeline);
    auto sampler = hog::render::make_sampler(device, VK_FILTER_NEAREST);
    REQUIRE(sampler);
    REQUIRE(device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, target->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::begin_rendering(commands, target->view(), extent, VkClearColorValue{});
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());
        hog::render::push_sampled_image(commands, *pipeline, 0, image.view(), sampler->handle());
        vkCmdPushConstants(commands, pipeline->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(layer), &layer);
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRendering(commands);
    }));
    auto pixels = hog::render::read_pixels(device, *target, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    return std::move(pixels).value();
}

} // namespace

TEST_CASE("only 4-byte colours and heights are streamed", "[render]") {
    CHECK(hog::render::texel_bytes(VK_FORMAT_R8G8B8A8_UNORM) == 4U);
    CHECK(hog::render::texel_bytes(VK_FORMAT_R32_SFLOAT) == 4U);
    CHECK(hog::render::texel_bytes(VK_FORMAT_B8G8R8A8_SRGB) == 4U);
    CHECK_FALSE(hog::render::texel_bytes(VK_FORMAT_R8G8B8_UNORM).has_value());
    CHECK_FALSE(hog::render::texel_bytes(VK_FORMAT_D32_SFLOAT).has_value());
}

TEST_CASE("read-back pixels lose their alpha and come out red first", "[render]") {
    const std::vector<std::uint8_t> two{1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(
        hog::render::to_rgb(two, VK_FORMAT_R8G8B8A8_UNORM).value_or(std::vector<std::uint8_t>{}) ==
        std::vector<std::uint8_t>{1, 2, 3, 5, 6, 7});
    CHECK(hog::render::to_rgb(two, VK_FORMAT_B8G8R8A8_SRGB).value_or(std::vector<std::uint8_t>{}) ==
          std::vector<std::uint8_t>{3, 2, 1, 7, 6, 5});
    CHECK(hog::render::to_rgb(two, VK_FORMAT_R16G16B16A16_SFLOAT).error().code ==
          hog::ErrorCode::Unsupported);
}

TEST_CASE("a streamed image takes each upload whole and refuses the wrong size", "[render][gpu]") {
    const auto device = test_device();
    constexpr VkExtent2D extent{.width = 4, .height = 2};
    constexpr std::size_t size = std::size_t{4} * 4 * 2;
    CHECK(make_streamed_image(*device, VK_FORMAT_R8G8B8_UNORM, extent, 2).error().code ==
          hog::ErrorCode::Unsupported);
    CHECK(make_streamed_image(*device, VK_FORMAT_R8G8B8A8_UNORM, {.width = 0, .height = 2}, 2)
              .error()
              .code == hog::ErrorCode::InvalidArgument);

    auto streamed = make_streamed_image(*device, VK_FORMAT_R8G8B8A8_UNORM, extent, 2);
    REQUIRE(streamed);
    CHECK_FALSE(streamed->ready());
    const std::vector<std::byte> first = filled(size, 0);
    const std::vector<std::byte> second = filled(size, 100);
    REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
        CHECK(streamed->update(commands, 0, first));
        CHECK(streamed->update(commands, 2, first).error().code == hog::ErrorCode::InvalidArgument);
        CHECK(streamed->update(commands, 1, std::span{first}.first(size - 4)).error().code ==
              hog::ErrorCode::InvalidArgument);
    }));
    CHECK(streamed->ready());
    REQUIRE(device->submit_now(
        [&](VkCommandBuffer commands) { CHECK(streamed->update(commands, 1, second)); }));

    const auto pixels =
        hog::render::read_pixels(*device, streamed->image(), hog::render::image_use::sampled);
    REQUIRE(pixels);
    REQUIRE(pixels->size() == size);
    for (std::size_t at = 0; at < size; ++at) {
        CHECK(static_cast<std::byte>(pixels->at(at)) == second[at]);
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("each layer of a streamed image is sampled on its own", "[render][gpu]") {
    const auto device = test_device();
    constexpr VkExtent2D extent{.width = 2, .height = 2};
    constexpr std::array<std::array<std::uint8_t, 4>, 3> colours{
        {{200, 0, 0, 255}, {0, 200, 0, 255}, {0, 0, 200, 255}}};
    CHECK(make_streamed_image(*device, VK_FORMAT_R8G8B8A8_UNORM, extent, 1, 0).error().code ==
          hog::ErrorCode::InvalidArgument);
    auto streamed = make_streamed_image(*device, VK_FORMAT_R8G8B8A8_UNORM, extent, 1, 3);
    REQUIRE(streamed);
    std::vector<std::byte> texels;
    for (const auto& colour : colours) {
        for (std::uint32_t texel = 0; texel < extent.width * extent.height; ++texel) {
            for (const std::uint8_t channel : colour) {
                texels.push_back(static_cast<std::byte>(channel));
            }
        }
    }
    REQUIRE(device->submit_now(
        [&](VkCommandBuffer commands) { CHECK(streamed->update(commands, 0, texels)); }));

    for (std::size_t layer = 0; layer < colours.size(); ++layer) {
        const std::vector<std::uint8_t> pixels =
            draw_layer(*device, streamed->image(), static_cast<float>(layer));
        REQUIRE(pixels.size() == std::size_t{4} * 4 * 4);
        for (std::size_t at = 0; at < pixels.size(); at += 4) {
            CHECK(std::array{pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]} ==
                  colours.at(layer));
        }
    }
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a filled image holds each mip of each layer as given", "[render][gpu]") {
    const auto device = test_device();
    const hog::render::ImageDesc desc{.format = VK_FORMAT_R8G8B8A8_UNORM,
                                      .extent = {.width = 4, .height = 2},
                                      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                      .layers = 2,
                                      .levels = 3};
    // 4 by 2, 2 by 1 and 1 by 1, two layers each.
    constexpr std::size_t size = std::size_t{4} * 2 * ((4 * 2) + (2 * 1) + (1 * 1));
    const std::vector<std::byte> texels = filled(size, 1);
    CHECK(hog::render::make_filled_image(*device, desc, std::span{texels}.first(size - 4))
              .error()
              .code == hog::ErrorCode::InvalidArgument);
    hog::render::ImageDesc too_many = desc;
    too_many.levels = 4;
    CHECK(hog::render::make_filled_image(*device, too_many, texels).error().code ==
          hog::ErrorCode::InvalidArgument);

    auto image = hog::render::make_filled_image(*device, desc, texels);
    REQUIRE(image);
    auto readback = hog::render::make_buffer(*device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             hog::render::Memory::Readback);
    REQUIRE(readback);
    REQUIRE(device->submit_now([&](VkCommandBuffer commands) {
        hog::render::transition(commands, image->handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                                hog::render::image_use::sampled,
                                hog::render::image_use::copy_source);
        std::vector<VkBufferImageCopy> regions;
        VkDeviceSize at = 0;
        for (std::uint32_t level = 0; level < desc.levels; ++level) {
            const VkExtent3D extent{.width = std::max(4U >> level, 1U),
                                    .height = std::max(2U >> level, 1U),
                                    .depth = 1};
            regions.push_back({.bufferOffset = at,
                               .bufferRowLength = 0,
                               .bufferImageHeight = 0,
                               .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                    .mipLevel = level,
                                                    .baseArrayLayer = 0,
                                                    .layerCount = desc.layers},
                               .imageOffset = {},
                               .imageExtent = extent});
            at += VkDeviceSize{4} * extent.width * extent.height * desc.layers;
        }
        vkCmdCopyImageToBuffer(commands, image->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback->handle(), static_cast<std::uint32_t>(regions.size()),
                               regions.data());
    }));
    const auto read = hog::render::read_back(*readback);
    REQUIRE(read);
    REQUIRE(read->size() == size);
    for (std::size_t at = 0; at < size; ++at) {
        CHECK(static_cast<std::byte>(read->at(at)) == texels[at]);
    }
    CHECK(hog::render::make_tiling_sampler(*device));
    CHECK(device->validation_errors() == 0);
}
