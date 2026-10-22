#include <hog/core/assert.hpp>
#include <hog/render/capsules.hpp>
#include <hog/render/device.hpp>
#include <hog/shaders/capsules.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace hog::render {

namespace {

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 feet_radius;
    glm::vec4 tall_night;
    glm::vec4 colour;
    glm::vec4 sun;
};

// Within what every device can push.
static_assert(sizeof(Push) <= 128);

// As capsules.slang has them.
constexpr std::uint32_t segments = 16;
constexpr std::uint32_t rings = 6;
constexpr std::uint32_t corner_count = ((2 * rings) + 1) * segments * 6;

constexpr VkDescriptorSetLayoutBinding
binding(std::uint32_t at, VkDescriptorType type,
        VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr};
}

// The sea's, read at the feet.
constexpr std::uint32_t sea_binding = 5;
constexpr std::array air_descriptors{
    binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
    binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
    binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
    binding(sea_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT),
    binding(sea_binding + 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_VERTEX_BIT),
    binding(sea_binding + 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_SHADER_STAGE_VERTEX_BIT)};

} // namespace

Result<Capsules> Capsules::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                                  bool air) {
    Capsules capsules;
    capsules.air_ = air;
    if (air) {
        auto calm = CalmSea::create(device);
        if (!calm) {
            return std::unexpected(std::move(calm).error());
        }
        capsules.calm_ = std::move(calm).value();
    }
    auto pipeline = make_pipeline(
        device, {.spirv = shaders::capsules,
                 .vertex_entry = air ? "vertex_air" : "vertex_main",
                 .fragment_entry = air ? "fragment_air" : "fragment_main",
                 .colour_format = colour_format,
                 .depth_format = depth_format,
                 .push_constant_bytes = sizeof(Push),
                 .descriptors = air ? std::span<const VkDescriptorSetLayoutBinding>{air_descriptors}
                                    : std::span<const VkDescriptorSetLayoutBinding>{}});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    capsules.pipeline_ = std::move(pipeline).value();
    return capsules;
}

std::vector<RippleBody> ripple_bodies(std::span<const Capsule> capsules, const GridPlacing& placing,
                                      const terrain::Bend& bend) {
    const DVec3 eye = bend.up * (bend.radius + bend.eye_height);
    const DVec3 up = glm::cross(placing.east, placing.north);
    struct Near {
        double distance;
        RippleBody body;
    };
    std::vector<Near> near;
    for (const Capsule& capsule : capsules) {
        const DVec3 feet = capsule.feet.metres();
        const DVec2 texel = grid_texel(placing, (glm::normalize(feet) * bend.radius) - eye);
        // However it leans.
        const double margin = (capsule.height + capsule.radius) / placing.spacing;
        if (glm::any(glm::lessThan(texel, DVec2{-margin})) ||
            glm::any(glm::greaterThan(texel, DVec2{ripple_size + margin}))) {
            continue;
        }
        const DVec3 tall = capsule.up * capsule.height;
        near.push_back(
            {.distance = glm::distance(feet, eye),
             .body = {.feet = glm::vec2{texel},
                      .above = static_cast<float>(glm::length(feet) - bend.radius),
                      .radius = static_cast<float>(capsule.radius),
                      .tall = glm::vec3{glm::dot(tall, placing.east), glm::dot(tall, placing.north),
                                        glm::dot(tall, up)}}});
    }
    const auto kept = static_cast<std::ptrdiff_t>(std::min(near.size(), ripple_most_bodies));
    std::ranges::partial_sort(near, near.begin() + kept, {}, &Near::distance);
    std::vector<RippleBody> bodies;
    bodies.reserve(static_cast<std::size_t>(kept));
    for (const Near& body : std::span{near}.first(static_cast<std::size_t>(kept))) {
        bodies.push_back(body.body);
    }
    return bodies;
}

void Capsules::draw(VkCommandBuffer commands, VkExtent2D extent, const CapsuleView& view,
                    std::span<const Capsule> capsules) const {
    if (extent.width == 0 || extent.height == 0 || capsules.empty()) {
        return;
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const Mat4 clip_from_eye{view_projection(view.camera, aspect)};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    if (air_) {
        HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE,
                   "drawing capsules lit by the air without it");
        push_uniform_buffer(commands, pipeline_, 0, view.air.air);
        push_uniform_buffer(commands, pipeline_, 1, view.air.lighting);
        push_sampled_image(commands, pipeline_, 2, view.air.transmittance, view.air.sampler);
        push_sampled_image(commands, pipeline_, 3, view.air.aerial, view.air.sampler);
        push_sampled_image(commands, pipeline_, 4, view.air.cloud_shadow, view.air.sampler);
        const SeaBindings sea = view.sea.waves != VK_NULL_HANDLE ? view.sea : calm_.bindings();
        push_uniform_buffer(commands, pipeline_, sea_binding, sea.waves);
        push_sampled_image(commands, pipeline_, sea_binding + 1, sea.shore, sea.sampler);
        push_sampled_image(commands, pipeline_, sea_binding + 2, sea.fetches, sea.sampler);
    }
    for (const Capsule& capsule : capsules) {
        // Bent at its feet and its top, and upright between, too short to bend itself.
        const DVec3 unbent_feet = capsule.feet - view.camera.eye;
        const DVec3 feet = terrain::bent(view.bend, unbent_feet);
        const DVec3 tall =
            terrain::bent(view.bend, unbent_feet + (capsule.up * capsule.height)) - feet;
        const Push push{
            .clip_from_eye = clip_from_eye,
            .feet_radius = glm::vec4{Vec3{feet}, static_cast<float>(capsule.radius)},
            .tall_night = glm::vec4{Vec3{tall}, static_cast<float>(view.night)},
            .colour = glm::vec4{capsule.colour, static_cast<float>(view.bend.eye_height)},
            .sun = glm::vec4{Vec3{view.sun}, static_cast<float>(glm::length(capsule.feet.metres()) -
                                                                view.bend.radius)}};
        vkCmdPushConstants(commands, pipeline_.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(Push), &push);
        vkCmdDraw(commands, corner_count, 1, 0, 0);
    }
}

} // namespace hog::render
