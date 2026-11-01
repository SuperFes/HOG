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

void Blocks::place(std::uint64_t key, std::shared_ptr<const terrain::BlockMesh> mesh) {
    remove(key);
    if (mesh && !mesh->indices.empty()) {
        Placed placed;
        placed.mesh = std::move(mesh);
        placed_.emplace(key, std::move(placed));
    }
}

Result<> Blocks::upload(Placed& placed) const {
    const terrain::BlockMesh& mesh = *placed.mesh;
    auto vertices = make_filled_buffer(*device_, std::as_bytes(std::span{mesh.vertices}),
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vertices) {
        return std::unexpected(std::move(vertices).error());
    }
    auto indices = make_filled_buffer(*device_, std::as_bytes(std::span{mesh.indices}),
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!indices) {
        return std::unexpected(std::move(indices).error());
    }
    placed.vertices = std::move(vertices).value();
    placed.indices = std::move(indices).value();
    placed.count = static_cast<std::uint32_t>(mesh.indices.size());
    placed.origin = mesh.origin;
    placed.mesh.reset();
    return {};
}

void Blocks::remove(std::uint64_t key) {
    if (const auto held = placed_.find(key); held != placed_.end()) {
        retire(std::move(held->second));
        placed_.erase(held);
    }
}

void Blocks::retire(Placed placed) {
    retiring_.emplace_back(draws_, std::move(placed));
}

void Blocks::draw_one(VkCommandBuffer commands, const Buffer& vertices, const Buffer& indices,
                      std::uint32_t count, WorldPosition origin, const BlockView& view,
                      double aspect) const {
    const Push push{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
                    .origin = glm::vec4{Vec3{origin - view.camera.eye}, 0.0F},
                    .up_height =
                        glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
                    .sun_radius = glm::vec4{Vec3{view.sun}, static_cast<float>(view.bend.radius)},
                    .faked_night = glm::vec4{static_cast<float>(view.bend.faked),
                                             static_cast<float>(view.night), 0.0F, 0.0F}};
    const std::array buffers{vertices.handle()};
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, buffers.data(), &offset);
    vkCmdBindIndexBuffer(commands, indices.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push),
                       &push);
    vkCmdDrawIndexed(commands, count, 1, 0, 0, 0);
}

Result<> Blocks::draw(VkCommandBuffer commands, VkExtent2D extent, const BlockView& view,
                      std::uint32_t frame) {
    ++draws_;
    // Each frame in flight has been drawn again since: none can still be reading them.
    std::erase_if(retiring_,
                  [&](const auto& retired) { return draws_ - retired.first > copies_.size(); });
    Copy& kept = copies_.at(frame);
    if (kept.shown != shown_) {
        if (auto copied = copy(kept); !copied) {
            return copied;
        }
    }
    for (auto& [key, placed] : placed_) {
        if (placed.mesh) {
            if (auto uploaded = upload(placed); !uploaded) {
                return uploaded;
            }
        }
    }
    if (extent.width == 0 || extent.height == 0 || (kept.count == 0 && placed_.empty())) {
        return {};
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    if (kept.count > 0) {
        draw_one(commands, kept.vertices, kept.indices, kept.count, kept.origin, view, aspect);
    }
    for (const auto& [key, placed] : placed_) {
        draw_one(commands, placed.vertices, placed.indices, placed.count, placed.origin, view,
                 aspect);
    }
    return {};
}

} // namespace hog::render
