#include <hog/core/assert.hpp>
#include <hog/render/clouds.hpp>
#include <hog/shaders/clouds.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace hog::render {

namespace {

struct Push {
    Mat4 eye_from_clip;
    glm::vec4 up_height;
    glm::vec4 radii;
    glm::vec4 moved_pixel;
};

static_assert(sizeof(Push) <= 128);

constexpr VkFormat map_format = VK_FORMAT_R8G8B8A8_UNORM;
// As clouds.slang's.
constexpr double depth_per_mm = 150.0;
constexpr double scattered_forward = 0.8;
constexpr double least_cover = 0.05;

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
                                 binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};

} // namespace

DVec3 within_period(DVec3 moved) noexcept {
    const auto within = [](double metres) {
        const double kept = std::fmod(metres, cloud_period);
        return kept < 0.0 ? kept + cloud_period : kept;
    };
    return {within(moved.x), within(moved.y), within(moved.z)};
}

CloudShade cloud_shade(double cover, double water) noexcept {
    const double covered = std::clamp(cover, 0.0, 1.0);
    if (covered <= 0.0) {
        return {};
    }
    const double thickness = depth_per_mm * std::max(water, 0.0) / std::max(covered, least_cover);
    const double unscattered = std::exp(-thickness);
    const double let_through = 1.0 / (1.0 + (0.75 * (1.0 - scattered_forward) * thickness));
    return {.direct = (1.0 - covered) + (covered * unscattered),
            .diffuse = covered * (let_through - unscattered)};
}

Result<Clouds> Clouds::create(const Device& device, VkFormat colour_format, std::uint32_t map_size,
                              std::uint32_t frames) {
    Clouds clouds;
    auto pipeline = make_pipeline(device, {.spirv = shaders::clouds,
                                           .colour_format = colour_format,
                                           .blend = Blend::Premultiplied,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = descriptors});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    clouds.pipeline_ = std::move(pipeline).value();
    // Depth isn't filtered.
    auto nearest = make_sampler(device, VK_FILTER_NEAREST);
    if (!nearest) {
        return std::unexpected(std::move(nearest).error());
    }
    clouds.nearest_ = std::move(nearest).value();
    auto linear = make_sampler(device, VK_FILTER_LINEAR);
    if (!linear) {
        return std::unexpected(std::move(linear).error());
    }
    clouds.linear_ = std::move(linear).value();
    const VkExtent2D extent{.width = map_size, .height = map_size};
    for (StreamedImage* map : {&clouds.before_, &clouds.after_}) {
        auto made = make_streamed_image(device, map_format, extent, frames, 6, true);
        if (!made) {
            return std::unexpected(std::move(made).error());
        }
        *map = std::move(made).value();
    }
    clouds.map_size_ = map_size;
    return clouds;
}

std::size_t Clouds::map_bytes() const noexcept {
    return std::size_t{6} * 4 * map_size_ * map_size_;
}

Result<> Clouds::update(VkCommandBuffer commands, std::uint32_t frame,
                        std::span<const std::byte> before, std::span<const std::byte> after) {
    if (auto updated = before_.update(commands, frame, before); !updated) {
        return updated;
    }
    return after_.update(commands, frame, after);
}

void Clouds::draw(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
                  const CloudsView& view) const noexcept {
    if (extent.width == 0 || extent.height == 0 || !ready()) {
        return;
    }
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing clouds without the air");
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const double pixel = 2.0 * std::tan(0.5 * view.camera.vertical_fov) / extent.height;
    const Push push{
        .eye_from_clip = Mat4{glm::inverse(glm::dmat4{view_projection(view.camera, aspect)})},
        .up_height = {Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
        .radii = {static_cast<float>(view.bend.radius), static_cast<float>(view.bend.faked),
                  static_cast<float>(view.base), static_cast<float>(view.faded)},
        .moved_pixel = {Vec3{within_period(view.moved)}, static_cast<float>(pixel)}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, view.air.air);
    push_uniform_buffer(commands, pipeline_, 1, view.air.lighting);
    push_sampled_image(commands, pipeline_, 2, view.air.transmittance, view.air.sampler);
    push_sampled_image(commands, pipeline_, 3, view.air.aerial, view.air.sampler);
    push_sampled_image(commands, pipeline_, 4, depth, nearest_.handle());
    push_sampled_image(commands, pipeline_, 5, before_.image().view(), linear_.handle());
    push_sampled_image(commands, pipeline_, 6, after_.image().view(), linear_.handle());
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(commands, 3, 1, 0, 0);
}

} // namespace hog::render
