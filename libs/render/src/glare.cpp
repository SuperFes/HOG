#include <hog/core/assert.hpp>
#include <hog/render/glare.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/shaders/glare.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>

namespace hog::render {

namespace {

struct Push {
    Mat4 eye_from_clip;
    glm::vec4 towards_exposure;
    glm::vec4 irradiance;
    glm::vec4 disc;
};

static_assert(sizeof(Push) <= 128);

constexpr VkDescriptorSetLayoutBinding
binding(std::uint32_t at, VkDescriptorType type,
        VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr};
}

// The lighting tells the picture's pixels seen from in the water too.
constexpr std::array bindings{binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                              binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
                              binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                              binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};

} // namespace

double eye_spread(double degrees) noexcept {
    const double near = degrees + 0.02;
    return ((0.478 * 20.91) / (near * near * near)) + ((0.138 * 72.37) / (near * near));
}

double eye_spread_streaked(double degrees) noexcept {
    constexpr double reach = 10.0; // degrees
    const double near = degrees + 0.02;
    return (0.478 * 20.91) / (near * near * near) * std::exp(-degrees / reach);
}

double streaks(double turn) noexcept {
    // A streak each of an even share of the turn, set off within it and bright to a degree by
    // strides through them, which spread both evenly so they average out.
    constexpr int count = 96;
    constexpr double width = 0.12; // of a share, a streak's
    constexpr double streaked = 0.8;
    const double at = (turn - std::floor(turn)) * count;
    const int streak = static_cast<int>(at) % count;
    const auto stride = [&](int step) {
        return (static_cast<double>((streak * step) % count) + 0.5) / count;
    };
    const double brightness = 5.0 * std::pow(stride(59), 4.0);
    const double across = (at - std::floor(at) - (0.5 + (0.6 * (stride(37) - 0.5)))) / width;
    return (1.0 - streaked) + (streaked * brightness * std::exp(-across * across) /
                               (width * std::sqrt(std::numbers::pi)));
}

Result<Glare> Glare::create(Device& device, VkFormat colour_format) {
    Glare glare;
    auto pipeline = make_pipeline(device, {.spirv = shaders::glare,
                                           .colour_format = colour_format,
                                           .blend = Blend::Screen,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = bindings});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    glare.pipeline_ = std::move(pipeline).value();
    // Depth isn't filtered.
    auto sampler = make_sampler(device, VK_FILTER_NEAREST);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    glare.sampler_ = std::move(sampler).value();
    auto linear = make_sampler(device, VK_FILTER_LINEAR);
    if (!linear) {
        return std::unexpected(std::move(linear).error());
    }
    glare.linear_ = std::move(linear).value();
    constexpr std::array<std::byte, 4> cloudless{};
    auto filled =
        make_filled_image(device, {.format = VK_FORMAT_R8G8B8A8_UNORM}, std::span{cloudless});
    if (!filled) {
        return std::unexpected(std::move(filled).error());
    }
    glare.cloudless_ = std::move(filled).value();
    return glare;
}

void Glare::draw(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
                 const GlareView& view) const noexcept {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing the glare without the air");
    const DVec2 size{extent.width, extent.height};
    const double aspect = size.x / size.y;
    const glm::dmat4 clip_from_eye{view_projection(view.camera, aspect)};
    const glm::dvec4 light = clip_from_eye * glm::dvec4{view.towards, 0.0};
    const bool in_front = light.w > 0.0;
    const DVec2 pixel = in_front ? ((DVec2{light} / light.w) * 0.5 + 0.5) * size : DVec2{0.0};
    const double pixels_per_radian = 0.5 * size.y / std::tan(0.5 * view.camera.vertical_fov);
    const Push push{.eye_from_clip = Mat4{glm::inverse(clip_from_eye)},
                    .towards_exposure = {Vec3{view.towards}, static_cast<float>(view.exposure)},
                    .irradiance = {Vec3{view.irradiance}, 0.0F},
                    .disc = {static_cast<float>(pixel.x), static_cast<float>(pixel.y),
                             static_cast<float>(view.angular_radius * pixels_per_radian),
                             in_front ? 1.0F : 0.0F}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_sampled_image(commands, pipeline_, 0, depth, sampler_.handle());
    push_uniform_buffer(commands, pipeline_, 1, view.air.lighting);
    push_sampled_image(commands, pipeline_, 2, view.air.cloud_shadow, view.air.sampler);
    push_sampled_image(commands, pipeline_, 3,
                       view.clouds != VK_NULL_HANDLE ? view.clouds : cloudless_.view(),
                       linear_.handle());
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(commands, 3, 1, 0, 0);
}

} // namespace hog::render
