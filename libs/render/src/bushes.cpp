#include <hog/core/assert.hpp>
#include <hog/render/bushes.hpp>
#include <hog/render/device.hpp>
#include <hog/shaders/bushes.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <utility>

namespace hog::render {

namespace {

constexpr VkDeviceSize tile_bytes = VkDeviceSize{tile_trees} * sizeof(BushAnchor);
static_assert(tile_bytes % 256 == 0);

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 bend_up;
    glm::vec4 bend_radii;
    glm::vec4 wind;
    glm::vec4 origin;
};

static_assert(sizeof(Push) <= 128);

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type,
                                               VkShaderStageFlags stages) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr};
}

constexpr std::array descriptors{
    binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT),
    binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)};

struct Settings {
    glm::vec4 reach; // x metres from the eye the bushes have shrunk away by
};

} // namespace

Result<Bushes> Bushes::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                              double fade_far) {
    Bushes bushes;
    const Settings settings{.reach = {static_cast<float>(fade_far), 0.0F, 0.0F, 0.0F}};
    auto settings_buffer = make_filled_buffer(device, std::as_bytes(std::span{&settings, 1}),
                                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!settings_buffer) {
        return std::unexpected(std::move(settings_buffer).error());
    }
    bushes.settings_ = std::move(settings_buffer).value();

    auto pipeline = make_pipeline(device, {.spirv = shaders::bushes,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .cull = VK_CULL_MODE_NONE,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = descriptors});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    bushes.pipeline_ = std::move(pipeline).value();
    return bushes;
}

void Bushes::draw(VkCommandBuffer commands, VkExtent2D extent, const AnchorPool& pool,
                  std::span<const StandDraw> draws, const GrassView& view) const {
    if (extent.width == 0 || extent.height == 0 || draws.empty()) {
        return;
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    Push push{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
              .bend_up = glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
              .bend_radii =
                  glm::vec4{static_cast<float>(view.bend.radius),
                            static_cast<float>(view.bend.faked), static_cast<float>(view.seconds),
                            static_cast<float>(view.camera.vertical_fov / extent.height)},
              .wind = glm::vec4{Vec3{view.wind}, 0.0F},
              .origin = {}};
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr std::uint32_t bush_vertices = 18; // three crossed quads

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, settings_.handle());
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing bushes lit by the air without it");
    push_uniform_buffer(commands, pipeline_, 2, view.air.air);
    push_uniform_buffer(commands, pipeline_, 3, view.air.lighting);
    push_sampled_image(commands, pipeline_, 4, view.air.transmittance, view.air.sampler);
    push_sampled_image(commands, pipeline_, 5, view.air.aerial, view.air.sampler);
    push_sampled_image(commands, pipeline_, 6, view.air.cloud_shadow, view.air.sampler);
    vkCmdPushConstants(commands, pipeline_.layout(), stages, 0, offsetof(Push, wind), &push);
    for (const StandDraw& draw : draws) {
        HOG_ASSERT(draw.slot < pool.capacity(), "drawing bushes from a slot beyond the pool");
        if (draw.count == 0) {
            continue;
        }
        HOG_ASSERT(draw.first + draw.count <= tile_trees, "drawing past the tile's anchors");
        push_storage_buffer(commands, pipeline_, 1, pool.anchors(),
                            VkDeviceSize{draw.slot} * tile_bytes, tile_bytes);
        push.origin =
            glm::vec4{relative_to(draw.origin, view.camera.eye), std::bit_cast<float>(draw.first)};
        vkCmdPushConstants(commands, pipeline_.layout(), stages, offsetof(Push, wind),
                           sizeof(Push) - offsetof(Push, wind), &push.wind);
        vkCmdDraw(commands, bush_vertices, draw.count, 0, 0);
    }
}

} // namespace hog::render
