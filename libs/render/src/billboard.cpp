#include <hog/render/billboard.hpp>
#include <hog/render/device.hpp>
#include <hog/shaders/billboard.hpp>

#include <array>
#include <cstddef>
#include <utility>

namespace hog::render {

namespace {

struct Push {
    glm::mat4 clip_from_eye;
    glm::vec4 feet;
    glm::vec4 right;
    glm::vec4 up;
};

constexpr std::array picture_binding{
    VkDescriptorSetLayoutBinding{.binding = 0,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 .descriptorCount = 1,
                                 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                                 .pImmutableSamplers = nullptr}};

constexpr std::uint32_t corner_count = 6;

} // namespace

Result<Billboard> Billboard::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                                    VkExtent2D size, std::span<const std::byte> texels) {
    Billboard billboard;
    auto picture = make_streamed_image(device, VK_FORMAT_R8G8B8A8_SRGB, size, 1);
    if (!picture) {
        return std::unexpected(std::move(picture).error());
    }
    billboard.picture_ = std::move(picture).value();
    Result<> updated;
    if (auto submitted = device.submit_now([&](VkCommandBuffer commands) {
            updated = billboard.picture_.update(commands, 0, texels);
        });
        !submitted) {
        return std::unexpected(std::move(submitted).error());
    }
    if (!updated) {
        return std::unexpected(std::move(updated).error());
    }

    auto sampler = make_sampler(device, VK_FILTER_LINEAR);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    billboard.sampler_ = std::move(sampler).value();
    auto pipeline = make_pipeline(device, {.spirv = shaders::billboard,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = picture_binding});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    billboard.pipeline_ = std::move(pipeline).value();
    billboard.aspect_ = static_cast<double>(size.width) / static_cast<double>(size.height);
    return billboard;
}

void Billboard::draw(VkCommandBuffer commands, VkExtent2D extent, const Camera& camera,
                     const Standing& standing) const noexcept {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    const DVec3 feet = standing.feet - camera.eye;
    // Facing the eye across the ground, or the way the eye looks when straight above.
    DVec3 towards = -feet - (standing.up * glm::dot(-feet, standing.up));
    if (glm::length(towards) < 1e-6) {
        towards = -camera.forward;
    }
    const DVec3 right = glm::normalize(glm::cross(standing.up, towards));
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const Push push{.clip_from_eye = Mat4{view_projection(camera, aspect)},
                    .feet = glm::vec4{Vec3{feet}, 0.0F},
                    .right = glm::vec4{Vec3{right * (standing.height * aspect_ / 2.0)}, 0.0F},
                    .up = glm::vec4{Vec3{standing.up * standing.height}, 0.0F}};

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_sampled_image(commands, pipeline_, 0, picture_.image().view(), sampler_.handle());
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push),
                       &push);
    vkCmdDraw(commands, corner_count, 1, 0, 0);
}

} // namespace hog::render
