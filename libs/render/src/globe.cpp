#include <hog/render/cube_sphere.hpp>
#include <hog/render/device.hpp>
#include <hog/render/globe.hpp>
#include <hog/shaders/globe.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace hog::render {

namespace {

struct Vertex {
    Vec3 position;
    Vec3 face_point;
};

struct Push {
    Mat4 clip_from_planet;
    glm::vec4 sun;
    float twilight = 0.0F;
};

// A host-written buffer the GPU reads from, holding `bytes`.
Result<Buffer> upload(const Device& device, std::span<const std::byte> bytes,
                      VkBufferUsageFlags usage) {
    auto buffer = make_buffer(device, bytes.size(), usage, Memory::Upload);
    if (!buffer) {
        return buffer;
    }
    std::ranges::copy(bytes, buffer->mapped().begin());
    if (auto flushed = buffer->flush(); !flushed) {
        return std::unexpected(std::move(flushed).error());
    }
    return buffer;
}

Push push_for(const GlobeView& view, VkExtent2D extent) {
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const glm::dmat4 planet_from_unit =
        glm::scale(glm::translate(glm::dmat4{1.0}, -view.camera.eye.metres()), DVec3{view.radius});
    return {.clip_from_planet =
                Mat4{glm::dmat4{view_projection(view.camera, aspect)} * planet_from_unit},
            .sun = glm::vec4{Vec3{view.sun}, static_cast<float>(view.night_light)},
            .twilight = static_cast<float>(std::max(view.twilight, 1e-6))};
}

} // namespace

Result<Globe> Globe::create(const Device& device, VkFormat colour_format, int cells) {
    const Mesh mesh = cube_sphere(cells);
    std::vector<Vertex> vertices(mesh.positions.size());
    for (std::size_t at = 0; at < vertices.size(); ++at) {
        vertices[at] = {.position = mesh.positions[at], .face_point = mesh.face_points[at]};
    }

    Globe globe;
    auto vertex_buffer =
        upload(device, std::as_bytes(std::span{vertices}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vertex_buffer) {
        return std::unexpected(std::move(vertex_buffer).error());
    }
    globe.vertices_ = std::move(vertex_buffer).value();
    auto index_buffer =
        upload(device, std::as_bytes(std::span{mesh.indices}), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!index_buffer) {
        return std::unexpected(std::move(index_buffer).error());
    }
    globe.indices_ = std::move(index_buffer).value();
    globe.index_count_ = static_cast<std::uint32_t>(mesh.indices.size());

    auto sampler = make_sampler(device, VK_FILTER_LINEAR);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    globe.sampler_ = std::move(sampler).value();

    const std::array bindings{VkVertexInputBindingDescription{
        .binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
    const std::array attributes{
        VkVertexInputAttributeDescription{.location = 0,
                                          .binding = 0,
                                          .format = VK_FORMAT_R32G32B32_SFLOAT,
                                          .offset = offsetof(Vertex, position)},
        VkVertexInputAttributeDescription{.location = 1,
                                          .binding = 0,
                                          .format = VK_FORMAT_R32G32B32_SFLOAT,
                                          .offset = offsetof(Vertex, face_point)}};
    const std::array descriptors{
        VkDescriptorSetLayoutBinding{.binding = 0,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .descriptorCount = 1,
                                     .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                     .pImmutableSamplers = nullptr}};
    auto pipeline = make_pipeline(device, {.spirv = shaders::globe,
                                           .bindings = bindings,
                                           .attributes = attributes,
                                           .colour_format = colour_format,
                                           .cull = VK_CULL_MODE_BACK_BIT,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = descriptors});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    globe.pipeline_ = std::move(pipeline).value();
    return globe;
}

void Globe::draw(VkCommandBuffer commands, VkExtent2D extent, const Image& faces,
                 const GlobeView& view) const noexcept {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    const Push push = push_for(view, extent);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_sampled_image(commands, pipeline_, 0, faces.view(), sampler_.handle());
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    const std::array vertices{vertices_.handle()};
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, vertices.data(), &offset);
    vkCmdBindIndexBuffer(commands, indices_.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commands, index_count_, 1, 0, 0, 0);
}

} // namespace hog::render
