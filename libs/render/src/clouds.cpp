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
    glm::vec4 veil_fog;
};

static_assert(sizeof(Push) <= 128);

// As clouds.slang's.
constexpr double depth_per_mm = 150.0;
constexpr double scattered_forward = 0.8;
constexpr double least_cover = 0.05;
constexpr double rain_extinction_per_metre = 2.5e-4;
constexpr double rain_extinction_power = 0.63;
constexpr double snow_extinction = 12.0;
// As weather.slang's.
constexpr double least_sun = 0.05;
// Of the sun's light the mist takes, the share it scatters on down.
constexpr double mist_scattered_down = 0.7;

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
                                 binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                 binding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};

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

CloudShade under_mist(CloudShade shade, double humidity, double fog_top, double height,
                      double sun_mu) noexcept {
    const double through =
        std::exp(-mist_above(humidity, fog_top, height) / std::max(sun_mu, least_sun));
    return {.direct = shade.direct * through,
            .diffuse = shade.diffuse + (shade.direct * (1.0 - through) * mist_scattered_down)};
}

double rain_extinction(double falling, double snow) noexcept {
    if (falling <= 0.0) {
        return 0.0;
    }
    return rain_extinction_per_metre * std::pow(falling, rain_extinction_power) *
           (1.0 + ((snow_extinction - 1.0) * std::clamp(snow, 0.0, 1.0)));
}

Result<Clouds> Clouds::create(const Device& device, VkFormat colour_format) {
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
    return clouds;
}

void Clouds::draw(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
                  const CloudsView& view) const noexcept {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing clouds without the air");
    HOG_ASSERT(view.weather.weather_before != VK_NULL_HANDLE, "drawing clouds without weather");
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const double pixel = 2.0 * std::tan(0.5 * view.camera.vertical_fov) / extent.height;
    const Push push{
        .eye_from_clip = Mat4{glm::inverse(glm::dmat4{view_projection(view.camera, aspect)})},
        .up_height = {Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
        .radii = {static_cast<float>(view.bend.radius), static_cast<float>(view.bend.faked),
                  static_cast<float>(view.base), static_cast<float>(view.weather.faded)},
        .moved_pixel = {Vec3{within_period(view.moved)}, static_cast<float>(pixel)},
        .veil_fog = {view.weather.falling ? 1.0F : 0.0F, static_cast<float>(view.weather.fog_top),
                     0.0F, 0.0F}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, view.air.air);
    push_uniform_buffer(commands, pipeline_, 1, view.air.lighting);
    push_sampled_image(commands, pipeline_, 2, view.air.transmittance, view.air.sampler);
    push_sampled_image(commands, pipeline_, 3, view.air.aerial, view.air.sampler);
    push_sampled_image(commands, pipeline_, 4, depth, nearest_.handle());
    push_sampled_image(commands, pipeline_, 5, view.weather.weather_before, view.weather.sampler);
    push_sampled_image(commands, pipeline_, 6, view.weather.weather_after, view.weather.sampler);
    push_sampled_image(commands, pipeline_, 7, view.weather.sky_before, view.weather.sampler);
    push_sampled_image(commands, pipeline_, 8, view.weather.sky_after, view.weather.sampler);
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(commands, 3, 1, 0, 0);
}

} // namespace hog::render
