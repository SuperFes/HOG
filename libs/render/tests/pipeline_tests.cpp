#include "test_device.hpp"

#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>
#include <hog/render/sampler.hpp>
#include <hog/shaders/dispatch.hpp>
#include <hog/shaders/fill.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

using hog::render::Blend;
using hog::render::Pipeline;
using hog::render::testing::test_device;

namespace {

using Pixel = std::array<std::uint8_t, 4>;

constexpr VkExtent2D extent{.width = 4, .height = 4};

struct Fill {
    std::array<float, 4> colour{};
    float depth = 0.5F;
};

struct Draw {
    const Pipeline* pipeline = nullptr;
    Fill fill;
};

Pipeline fill_pipeline(const hog::render::Device& device, Blend blend, bool depth_write = true) {
    auto pipeline = hog::render::make_pipeline(device, {.spirv = hog::shaders::fill,
                                                        .depth_format = VK_FORMAT_D32_SFLOAT,
                                                        .depth_write = depth_write,
                                                        .blend = blend,
                                                        .push_constant_bytes = sizeof(Fill)});
    REQUIRE(pipeline);
    return std::move(pipeline).value();
}

// Clears to `background` and the farthest depth, then draws each in turn; returns the middle pixel.
Pixel draw_all(hog::render::Device& device, std::array<float, 4> background,
               std::span<const Draw> draws) {
    auto colour = hog::render::make_image(
        device, {.format = VK_FORMAT_R8G8B8A8_UNORM,
                 .extent = extent,
                 .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    REQUIRE(colour);
    auto depth =
        hog::render::make_image(device, {.format = VK_FORMAT_D32_SFLOAT,
                                         .extent = extent,
                                         .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT});
    REQUIRE(depth);
    const auto drawn = device.submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        hog::render::transition(commands, colour->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::colour_target);
        hog::render::transition(commands, depth->handle(), VK_IMAGE_ASPECT_DEPTH_BIT, use::discard,
                                use::depth_target);
        hog::render::begin_rendering(commands, colour->view(), extent,
                                     VkClearColorValue{.float32 = {background[0], background[1],
                                                                   background[2], background[3]}},
                                     {.view = depth->view()});
        for (const Draw& draw : draws) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->handle());
            vkCmdPushConstants(commands, draw.pipeline->layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(Fill), &draw.fill);
            vkCmdDraw(commands, 3, 1, 0, 0);
        }
        vkCmdEndRendering(commands);
    });
    REQUIRE(drawn);
    const auto pixels =
        hog::render::read_pixels(device, *colour, hog::render::image_use::colour_target);
    REQUIRE(pixels);
    const std::size_t middle = std::size_t{4} * ((std::size_t{2} * extent.width) + 2);
    return {pixels->at(middle), pixels->at(middle + 1), pixels->at(middle + 2),
            pixels->at(middle + 3)};
}

constexpr std::array<float, 4> black{0.0F, 0.0F, 0.0F, 1.0F};
constexpr std::array<float, 4> red{1.0F, 0.0F, 0.0F, 1.0F};
constexpr std::array<float, 4> green{0.0F, 1.0F, 0.0F, 1.0F};

} // namespace

TEST_CASE("depth keeps what's nearest, whichever is drawn first", "[render][gpu]") {
    const auto device = test_device();
    const Pipeline opaque = fill_pipeline(*device, Blend::None);
    // Reversed, so the greater depth is nearer.
    const Draw near{.pipeline = &opaque, .fill = {.colour = red, .depth = 0.8F}};
    const Draw far{.pipeline = &opaque, .fill = {.colour = green, .depth = 0.2F}};
    CHECK(draw_all(*device, black, std::array{near, far}) == Pixel{255, 0, 0, 255});
    CHECK(draw_all(*device, black, std::array{far, near}) == Pixel{255, 0, 0, 255});
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("colour is laid over what's drawn by its alpha, premultiplied or not, or added to it",
          "[render][gpu]") {
    const auto device = test_device();
    const Pipeline alpha = fill_pipeline(*device, Blend::Alpha);
    const Pipeline premultiplied = fill_pipeline(*device, Blend::Premultiplied);
    const Pipeline add = fill_pipeline(*device, Blend::Add);
    const std::array<float, 4> blue{0.0F, 0.0F, 0.4F, 1.0F};

    const Pixel over = draw_all(
        *device, blue,
        std::array{Draw{.pipeline = &alpha, .fill = {.colour = {1.0F, 0.0F, 0.0F, 0.5F}}}});
    // Half of 255 may round either way.
    CHECK(over[0] >= 127);
    CHECK(over[0] <= 128);
    CHECK(over[1] == 0);
    CHECK(over[2] == 51);
    CHECK(over[3] == 255);

    // Brighter than its alpha alone would let it be laid over.
    const Pixel glowing = draw_all(
        *device, blue,
        std::array{Draw{.pipeline = &premultiplied, .fill = {.colour = {0.6F, 0.0F, 0.0F, 0.5F}}}});
    CHECK(glowing == Pixel{153, 0, 51, 255});

    const Pixel added =
        draw_all(*device, blue,
                 std::array{Draw{.pipeline = &add, .fill = {.colour = {0.4F, 0.0F, 0.2F, 0.0F}}}});
    CHECK(added == Pixel{102, 0, 153, 255});
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("drawing without writing depth leaves it for what's drawn after", "[render][gpu]") {
    const auto device = test_device();
    const Pipeline unwritten = fill_pipeline(*device, Blend::None, false);
    const Pipeline opaque = fill_pipeline(*device, Blend::None);
    const std::array draws{Draw{.pipeline = &unwritten, .fill = {.colour = red, .depth = 0.9F}},
                           Draw{.pipeline = &opaque, .fill = {.colour = green, .depth = 0.1F}}};
    CHECK(draw_all(*device, black, draws) == Pixel{0, 255, 0, 255});
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("compute shaders write images and buffers, and sample what they wrote", "[render][gpu]") {
    const auto device = test_device();
    constexpr std::array<std::uint32_t, 2> size{12, 5};
    constexpr std::size_t count = std::size_t{size[0]} * size[1];
    constexpr std::array<VkDescriptorSetLayoutBinding, 2> writing{
        VkDescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     .descriptorCount = 1,
                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                     .pImmutableSamplers = nullptr},
        VkDescriptorSetLayoutBinding{.binding = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     .descriptorCount = 1,
                                     .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                     .pImmutableSamplers = nullptr}};
    std::array<VkDescriptorSetLayoutBinding, 2> sampling = writing;
    sampling[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    auto write = hog::render::make_compute_pipeline(*device, {.spirv = hog::shaders::dispatch,
                                                              .push_constant_bytes = sizeof(size),
                                                              .descriptors = writing});
    REQUIRE(write);
    auto sample = hog::render::make_compute_pipeline(*device, {.spirv = hog::shaders::dispatch,
                                                               .entry = "sample_main",
                                                               .push_constant_bytes = sizeof(size),
                                                               .descriptors = sampling});
    REQUIRE(sample);
    CHECK(write->bind_point() == VK_PIPELINE_BIND_POINT_COMPUTE);

    auto image = hog::render::make_image(
        *device, {.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                  .extent = {.width = size[0], .height = size[1]},
                  .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
    REQUIRE(image);
    const auto sampler = hog::render::make_sampler(*device, VK_FILTER_NEAREST);
    REQUIRE(sampler);
    auto indices =
        hog::render::make_buffer(*device, count * sizeof(std::uint32_t),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(indices);
    auto samples =
        hog::render::make_buffer(*device, count * sizeof(std::array<float, 4>),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hog::render::Memory::Readback);
    REQUIRE(samples);

    const auto run = device->submit_now([&](VkCommandBuffer commands) {
        namespace use = hog::render::image_use;
        const auto groups = [](std::uint32_t across) { return (across + 7) / 8; };
        hog::render::transition(commands, image->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                                use::storage);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, write->handle());
        vkCmdPushConstants(commands, write->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size),
                           size.data());
        hog::render::push_storage_image(commands, *write, 0, image->view());
        hog::render::push_storage_buffer(commands, *write, 1, indices->handle());
        vkCmdDispatch(commands, groups(size[0]), groups(size[1]), 1);

        hog::render::transition(commands, image->handle(), VK_IMAGE_ASPECT_COLOR_BIT, use::storage,
                                use::sampled);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, sample->handle());
        vkCmdPushConstants(commands, sample->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size),
                           size.data());
        hog::render::push_sampled_image(commands, *sample, 0, image->view(), sampler->handle());
        hog::render::push_storage_buffer(commands, *sample, 1, samples->handle());
        vkCmdDispatch(commands, groups(size[0]), groups(size[1]), 1);

        const VkMemoryBarrier2 to_host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                       .pNext = nullptr,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                       .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .pNext = nullptr,
                                          .dependencyFlags = 0,
                                          .memoryBarrierCount = 1,
                                          .pMemoryBarriers = &to_host,
                                          .bufferMemoryBarrierCount = 0,
                                          .pBufferMemoryBarriers = nullptr,
                                          .imageMemoryBarrierCount = 0,
                                          .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(commands, &dependency);
    });
    REQUIRE(run);
    REQUIRE(indices->invalidate());
    REQUIRE(samples->invalidate());

    std::vector<std::uint32_t> written(count);
    std::memcpy(written.data(), indices->mapped().data(), count * sizeof(std::uint32_t));
    std::vector<std::array<float, 4>> sampled(count);
    std::memcpy(sampled.data(), samples->mapped().data(), count * sizeof(std::array<float, 4>));
    for (std::uint32_t y = 0; y < size[1]; ++y) {
        for (std::uint32_t x = 0; x < size[0]; ++x) {
            const std::size_t at = (std::size_t{y} * size[0]) + x;
            INFO("texel " << x << ", " << y);
            CHECK(written[at] == at);
            // Half floats hold these to within a part in a thousand.
            CHECK(std::abs(sampled[at][0] - (static_cast<float>(x) / size[0])) < 1e-3F);
            CHECK(std::abs(sampled[at][1] - (static_cast<float>(y) / size[1])) < 1e-3F);
            CHECK(sampled[at][2] == 0.5F);
            CHECK(sampled[at][3] == 1.0F);
        }
    }
    CHECK(device->validation_errors() == 0);
}
