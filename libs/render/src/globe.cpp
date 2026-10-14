#include <hog/render/cube_sphere.hpp>
#include <hog/render/device.hpp>
#include <hog/render/globe.hpp>
#include <hog/shaders/globe.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace hog::render {

namespace {

struct Vertex {
    Vec3 position;
    Vec3 face_point;
    Vec3 along_s;
    Vec3 along_t;
};

struct Push {
    Mat4 clip_from_planet;
    glm::vec4 sun;
    float twilight = 0.0F;
    float relief = 0.0F;
    float relief_steps = 0.0F;
    float cloud_lift = 0.0F;
    float air_lift = 0.0F;
    alignas(16) glm::vec4 eye{};
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

Push push_for(const GlobeView& view, VkExtent2D extent, const Image& relief) {
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const glm::dmat4 planet_from_unit =
        glm::scale(glm::translate(glm::dmat4{1.0}, -view.camera.eye.metres()), DVec3{view.radius});
    return {.clip_from_planet =
                Mat4{glm::dmat4{view_projection(view.camera, aspect)} * planet_from_unit},
            .sun = glm::vec4{Vec3{view.sun}, static_cast<float>(view.night_light)},
            .twilight = static_cast<float>(std::max(view.twilight, 1e-6)),
            .relief = static_cast<float>(view.relief),
            .relief_steps = static_cast<float>(relief.desc().extent.width - 1),
            .cloud_lift = static_cast<float>(view.cloud_lift),
            .air_lift = static_cast<float>(view.air),
            .eye = glm::vec4{Vec3{view.camera.eye.metres() / view.radius}, 1.0F}};
}

constexpr VkVertexInputAttributeDescription attribute(std::uint32_t location,
                                                      std::uint32_t offset) {
    return {
        .location = location, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offset};
}

constexpr VkDescriptorSetLayoutBinding sampled_image(std::uint32_t binding,
                                                     VkShaderStageFlags stages) {
    return {.binding = binding,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr};
}

constexpr std::array vertex_bindings{VkVertexInputBindingDescription{
    .binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
constexpr std::array vertex_attributes{
    attribute(0, offsetof(Vertex, position)), attribute(1, offsetof(Vertex, face_point)),
    attribute(2, offsetof(Vertex, along_s)), attribute(3, offsetof(Vertex, along_t))};
constexpr std::array ground_images{
    sampled_image(0, VK_SHADER_STAGE_FRAGMENT_BIT),
    sampled_image(1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)};
constexpr std::array cloud_images{sampled_image(0, VK_SHADER_STAGE_FRAGMENT_BIT)};

enum class Layer : std::uint8_t { Ground, Clouds, Air };

// The ground's pipeline, or a shell's laid over it without hiding what's drawn after.
PipelineDesc globe_pipeline(VkFormat colour_format, VkFormat depth_format, Layer layer) {
    PipelineDesc desc{.spirv = shaders::globe,
                      .bindings = vertex_bindings,
                      .attributes = vertex_attributes,
                      .colour_format = colour_format,
                      .depth_format = depth_format,
                      .cull = VK_CULL_MODE_BACK_BIT,
                      .push_constant_bytes = sizeof(Push),
                      .descriptors = ground_images};
    if (layer == Layer::Ground) {
        return desc;
    }
    // The shells need only where they are.
    desc.attributes = std::span{vertex_attributes}.first(2);
    desc.depth_write = false;
    if (layer == Layer::Clouds) {
        desc.vertex_entry = "cloud_vertex";
        desc.fragment_entry = "cloud_fragment";
        desc.blend = Blend::Alpha;
        desc.descriptors = cloud_images;
    } else {
        desc.vertex_entry = "air_vertex";
        desc.fragment_entry = "air_fragment";
        desc.blend = Blend::Add;
        // Drawn from its far side, which shows from within it too.
        desc.cull = VK_CULL_MODE_FRONT_BIT;
        desc.descriptors = {};
    }
    return desc;
}

} // namespace

Result<Globe> Globe::create(const Device& device, VkFormat colour_format, VkFormat depth_format,
                            int cells) {
    const Mesh mesh = cube_sphere(cells);
    std::vector<Vertex> vertices(mesh.positions.size());
    for (std::size_t at = 0; at < vertices.size(); ++at) {
        vertices[at] = {.position = mesh.positions[at],
                        .face_point = mesh.face_points[at],
                        .along_s = mesh.along_s[at],
                        .along_t = mesh.along_t[at]};
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

    auto ground = make_pipeline(device, globe_pipeline(colour_format, depth_format, Layer::Ground));
    if (!ground) {
        return std::unexpected(std::move(ground).error());
    }
    globe.pipeline_ = std::move(ground).value();
    auto clouds = make_pipeline(device, globe_pipeline(colour_format, depth_format, Layer::Clouds));
    if (!clouds) {
        return std::unexpected(std::move(clouds).error());
    }
    globe.cloud_pipeline_ = std::move(clouds).value();
    auto air = make_pipeline(device, globe_pipeline(colour_format, depth_format, Layer::Air));
    if (!air) {
        return std::unexpected(std::move(air).error());
    }
    globe.air_pipeline_ = std::move(air).value();
    return globe;
}

void Globe::draw(VkCommandBuffer commands, VkExtent2D extent, const GlobeImages& images,
                 const GlobeView& view) const noexcept {
    if (extent.width == 0 || extent.height == 0 || images.faces == nullptr ||
        images.relief == nullptr) {
        return;
    }
    const Push push = push_for(view, extent, *images.relief);
    const std::array vertices{vertices_.handle()};
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, vertices.data(), &offset);
    vkCmdBindIndexBuffer(commands, indices_.handle(), 0, VK_INDEX_TYPE_UINT32);
    const auto draw_with = [&](const Pipeline& pipeline) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdPushConstants(commands, pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        vkCmdDrawIndexed(commands, index_count_, 1, 0, 0, 0);
    };

    push_sampled_image(commands, pipeline_, 0, images.faces->view(), sampler_.handle());
    push_sampled_image(commands, pipeline_, 1, images.relief->view(), sampler_.handle());
    draw_with(pipeline_);
    if (images.clouds != nullptr) {
        push_sampled_image(commands, cloud_pipeline_, 0, images.clouds->view(), sampler_.handle());
        draw_with(cloud_pipeline_);
    }
    if (view.air > 0.0) {
        draw_with(air_pipeline_);
    }
}

} // namespace hog::render
