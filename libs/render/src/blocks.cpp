#include <hog/render/blocks.hpp>
#include <hog/render/device.hpp>
#include <hog/shaders/blocks.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace hog::render {

namespace {

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 origin;
    glm::vec4 up_height;
    glm::vec4 sun_radius;
    glm::vec4 faked_night;
};

// Within what every device can push.
static_assert(sizeof(Push) <= 128);

constexpr std::array vertex_bindings{
    VkVertexInputBindingDescription{.binding = 0,
                                    .stride = sizeof(terrain::BlockVertex),
                                    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};

constexpr std::array vertex_attributes{
    VkVertexInputAttributeDescription{.location = 0,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32B32_SFLOAT,
                                      .offset = offsetof(terrain::BlockVertex, position)},
    VkVertexInputAttributeDescription{.location = 1,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32B32_SFLOAT,
                                      .offset = offsetof(terrain::BlockVertex, normal)},
    VkVertexInputAttributeDescription{.location = 2,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32B32_SFLOAT,
                                      .offset = offsetof(terrain::BlockVertex, colour)}};

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

Result<Blocks> Blocks::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                              std::uint32_t frames) {
    Blocks blocks;
    blocks.device_ = &device;
    blocks.copies_.resize(frames);
    auto pipeline = make_pipeline(device, {.spirv = shaders::blocks,
                                           .bindings = vertex_bindings,
                                           .attributes = vertex_attributes,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .push_constant_bytes = sizeof(Push)});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    blocks.pipeline_ = std::move(pipeline).value();
    return blocks;
}

void Blocks::show(std::shared_ptr<const terrain::BlockMesh> mesh) {
    mesh_ = std::move(mesh);
    ++shown_;
}

Result<> Blocks::copy(Copy& copy) const {
    copy.count = 0;
    copy.shown = shown_;
    if (!mesh_ || mesh_->indices.empty()) {
        return {};
    }
    if (auto held = hold(*device_, copy.vertices, std::as_bytes(std::span{mesh_->vertices}),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        !held) {
        return held;
    }
    if (auto held = hold(*device_, copy.indices, std::as_bytes(std::span{mesh_->indices}),
                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        !held) {
        return held;
    }
    copy.count = static_cast<std::uint32_t>(mesh_->indices.size());
    copy.origin = mesh_->origin;
    return {};
}

Result<> Blocks::draw(VkCommandBuffer commands, VkExtent2D extent, const BlockView& view,
                      std::uint32_t frame) {
    Copy& kept = copies_.at(frame);
    if (kept.shown != shown_) {
        if (auto copied = copy(kept); !copied) {
            return copied;
        }
    }
    if (extent.width == 0 || extent.height == 0 || kept.count == 0) {
        return {};
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const Push push{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
                    .origin = glm::vec4{Vec3{kept.origin - view.camera.eye}, 0.0F},
                    .up_height =
                        glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
                    .sun_radius = glm::vec4{Vec3{view.sun}, static_cast<float>(view.bend.radius)},
                    .faked_night = glm::vec4{static_cast<float>(view.bend.faked),
                                             static_cast<float>(view.night), 0.0F, 0.0F}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
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
