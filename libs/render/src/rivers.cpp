#include <hog/core/assert.hpp>
#include <hog/render/device.hpp>
#include <hog/render/rivers.hpp>
#include <hog/shaders/river.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace hog::render {

namespace {

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 origin_seconds;
    glm::vec4 up_height;
    glm::vec4 sun_radius;
    glm::vec4 faked;
};

// Within what every device can push.
static_assert(sizeof(Push) <= 128);

// The flow's cycles, as river.slang counts them, repeat within this many seconds, kept short for
// the precision of a float.
constexpr double cycles_repeat = 1'600.0;

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr};
}

constexpr std::array descriptors{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                 binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                 binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                 binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                 binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                 binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                 binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                 binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};

constexpr std::array vertex_bindings{
    VkVertexInputBindingDescription{.binding = 0,
                                    .stride = sizeof(terrain::RiverVertex),
                                    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};

constexpr std::array vertex_attributes{
    VkVertexInputAttributeDescription{.location = 0,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32B32_SFLOAT,
                                      .offset = offsetof(terrain::RiverVertex, position)},
    VkVertexInputAttributeDescription{.location = 1,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32_SFLOAT,
                                      .offset = offsetof(terrain::RiverVertex, beyond)},
    VkVertexInputAttributeDescription{.location = 2,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32B32_SFLOAT,
                                      .offset = offsetof(terrain::RiverVertex, flow)},
    VkVertexInputAttributeDescription{.location = 3,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32_SFLOAT,
                                      .offset = offsetof(terrain::RiverVertex, rough)},
    VkVertexInputAttributeDescription{.location = 4,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32B32_SFLOAT,
                                      .offset = offsetof(terrain::RiverVertex, outward)},
    VkVertexInputAttributeDescription{.location = 5,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32_SFLOAT,
                                      .offset = offsetof(terrain::RiverVertex, half)}};

// Upload memory for `bytes`, kept if `kept` holds as many already.
Result<> hold(const Device& device, Buffer& kept, std::span<const std::byte> bytes,
              VkBufferUsageFlags usage) {
    if (kept.size() < bytes.size()) {
        auto made = make_buffer(device, bytes.size(), usage, Memory::Upload);
        if (!made) {
            return std::unexpected(std::move(made).error());
        }
        kept = std::move(made).value();
    }
    std::memcpy(kept.mapped().data(), bytes.data(), bytes.size());
    return kept.flush();
}

} // namespace

Result<RiverSurface> RiverSurface::create(Device& device, VkFormat colour_format,
                                          VkFormat depth_format, std::uint32_t frames) {
    RiverSurface surface;
    surface.device_ = &device;
    surface.copies_.resize(frames);
    auto pipeline = make_pipeline(device, {.spirv = shaders::river,
                                           .vertex_entry = "vertex_main",
                                           .fragment_entry = "fragment_through",
                                           .bindings = vertex_bindings,
                                           .attributes = vertex_attributes,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = descriptors});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    surface.pipeline_ = std::move(pipeline).value();
    auto sampler = make_sampler(device, VK_FILTER_LINEAR);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    surface.sampler_ = std::move(sampler).value();
    return surface;
}

void RiverSurface::show(std::shared_ptr<const terrain::RiverWater> water) {
    water_ = std::move(water);
    ++shown_;
}

Result<> RiverSurface::copy(Copy& copy) const {
    copy.count = 0;
    copy.shown = shown_;
    if (!water_ || water_->indices.empty()) {
        return {};
    }
    if (auto held = hold(*device_, copy.vertices, std::as_bytes(std::span{water_->vertices}),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        !held) {
        return held;
    }
    if (auto held = hold(*device_, copy.indices, std::as_bytes(std::span{water_->indices}),
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        !held) {
        return held;
    }
    copy.count = static_cast<std::uint32_t>(water_->indices.size());
    copy.origin = water_->origin;
    return {};
}

Result<> RiverSurface::draw(VkCommandBuffer commands, VkExtent2D extent, const RiverView& view,
                            std::uint32_t frame) {
    Copy& kept = copies_.at(frame);
    if (kept.shown != shown_) {
        if (auto copied = copy(kept); !copied) {
            return copied;
        }
    }
    if (extent.width == 0 || extent.height == 0 || kept.count == 0 || !view.behind) {
        return {};
    }
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing rivers without the air");
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const Push push{
        .clip_from_eye = Mat4{view_projection(view.camera, aspect)},
        .origin_seconds = glm::vec4{Vec3{kept.origin - view.camera.eye},
                                    static_cast<float>(std::fmod(view.time, cycles_repeat))},
        .up_height = glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
        .sun_radius = glm::vec4{Vec3{view.sun}, static_cast<float>(view.bend.radius)},
        .faked = glm::vec4{
            static_cast<float>(view.bend.faked),
            static_cast<float>(view.camera.vertical_fov / static_cast<double>(extent.height)), 0.0F,
            0.0F}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, view.air.air);
    push_uniform_buffer(commands, pipeline_, 1, view.air.lighting);
    push_sampled_image(commands, pipeline_, 2, view.air.transmittance, view.air.sampler);
    push_sampled_image(commands, pipeline_, 3, view.air.sky_view, view.air.sampler);
    push_sampled_image(commands, pipeline_, 4, view.air.aerial, view.air.sampler);
    push_sampled_image(commands, pipeline_, 5, view.air.cloud_shadow, view.air.sampler);
    push_sampled_image(commands, pipeline_, 6, view.behind->colour, sampler_.handle());
    push_sampled_image(commands, pipeline_, 7, view.behind->depth, sampler_.handle());
    const std::array vertices{kept.vertices.handle()};
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, vertices.data(), &offset);
    vkCmdBindIndexBuffer(commands, kept.indices.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push),
                       &push);
    vkCmdDrawIndexed(commands, kept.count, 1, 0, 0, 0);
    return {};
}

} // namespace hog::render
