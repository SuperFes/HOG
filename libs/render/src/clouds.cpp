#include <hog/core/assert.hpp>
#include <hog/render/clouds.hpp>
#include <hog/shaders/cloud_march.hpp>
#include <hog/shaders/clouds.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace hog::render {

namespace {

struct MarchPush {
    Mat4 eye_from_clip;
    glm::vec4 up_height;
    glm::vec4 radii;
    glm::vec4 moved;
    glm::vec4 bounds;
};

struct Push {
    Mat4 eye_from_clip;
    glm::vec4 up_height;
    glm::vec4 radii;
    glm::vec4 veil_fog;
};

static_assert(sizeof(MarchPush) <= 128);
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

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type,
                                               VkShaderStageFlags stage) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = stage,
            .pImmutableSamplers = nullptr};
}

constexpr VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
constexpr VkShaderStageFlags fragment = VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr VkShaderStageFlags compute = VK_SHADER_STAGE_COMPUTE_BIT;

constexpr std::array descriptors{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, fragment),
                                 binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, fragment),
                                 binding(2, sampled, fragment),
                                 binding(3, sampled, fragment),
                                 binding(4, sampled, fragment),
                                 binding(5, sampled, fragment),
                                 binding(6, sampled, fragment),
                                 binding(7, sampled, fragment),
                                 binding(8, sampled, fragment),
                                 binding(9, sampled, fragment),
                                 binding(10, sampled, fragment)};

constexpr std::array march_descriptors{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, compute),
                                       binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, compute),
                                       binding(2, sampled, compute),
                                       binding(3, sampled, compute),
                                       binding(4, sampled, compute),
                                       binding(5, sampled, compute),
                                       binding(6, sampled, compute),
                                       binding(7, sampled, compute),
                                       binding(8, sampled, compute),
                                       binding(9, sampled, compute),
                                       binding(10, sampled, compute),
                                       binding(11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, compute),
                                       binding(12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, compute)};

Mat4 eye_from_clip(const Camera& camera, VkExtent2D extent) noexcept {
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    return Mat4{glm::inverse(glm::dmat4{view_projection(camera, aspect)})};
}

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

Result<Clouds> Clouds::create(Device& device, VkFormat colour_format, jobs::JobSystem& jobs) {
    Clouds clouds;
    auto march = make_compute_pipeline(device, {.spirv = shaders::cloud_march,
                                                .entry = "march_main",
                                                .push_constant_bytes = sizeof(MarchPush),
                                                .descriptors = march_descriptors});
    if (!march) {
        return std::unexpected(std::move(march).error());
    }
    clouds.march_ = std::move(march).value();
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
    auto tiling = make_tiling_sampler(device);
    if (!tiling) {
        return std::unexpected(std::move(tiling).error());
    }
    clouds.tiling_ = std::move(tiling).value();
    auto noise = make_cloud_noise(device, jobs);
    if (!noise) {
        return std::unexpected(std::move(noise).error());
    }
    clouds.noise_ = std::move(noise).value();
    return clouds;
}

VkExtent2D Clouds::march_extent(VkExtent2D extent) noexcept {
    return {.width = (extent.width + 1) / 2, .height = (extent.height + 1) / 2};
}

void Clouds::march(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
                   VkImageView marched, VkImageView distances,
                   const CloudsView& view) const noexcept {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "marching clouds without the air");
    HOG_ASSERT(view.weather.weather_before != VK_NULL_HANDLE, "marching clouds without weather");
    const double pixel = 2.0 * std::tan(0.5 * view.camera.vertical_fov) / extent.height;
    const MarchPush push{
        .eye_from_clip = eye_from_clip(view.camera, extent),
        .up_height = {Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
        .radii = {static_cast<float>(view.bend.radius), static_cast<float>(view.bend.faked),
                  static_cast<float>(view.weather.faded), static_cast<float>(pixel)},
        .moved = {Vec3{within_period(view.moved)}, 0.0F},
        .bounds = {static_cast<float>(view.weather.lowest_base),
                   static_cast<float>(view.weather.highest_top), 0.0F, 0.0F}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, march_.handle());
    push_uniform_buffer(commands, march_, 0, view.air.air);
    push_uniform_buffer(commands, march_, 1, view.air.lighting);
    push_sampled_image(commands, march_, 2, view.air.transmittance, view.air.sampler);
    push_sampled_image(commands, march_, 3, view.air.aerial, view.air.sampler);
    push_sampled_image(commands, march_, 4, depth, nearest_.handle());
    push_sampled_image(commands, march_, 5, view.weather.weather_before, view.weather.sampler);
    push_sampled_image(commands, march_, 6, view.weather.weather_after, view.weather.sampler);
    push_sampled_image(commands, march_, 7, view.weather.sky_before, view.weather.sampler);
    push_sampled_image(commands, march_, 8, view.weather.sky_after, view.weather.sampler);
    push_sampled_image(commands, march_, 9, noise_.shape.view(), tiling_.handle());
    push_sampled_image(commands, march_, 10, noise_.detail.view(), tiling_.handle());
    push_storage_image(commands, march_, 11, marched);
    push_storage_image(commands, march_, 12, distances);
    vkCmdPushConstants(commands, march_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                       &push);
    const VkExtent2D size = march_extent(extent);
    vkCmdDispatch(commands, (size.width + 7) / 8, (size.height + 7) / 8, 1);
}

void Clouds::draw(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
                  VkImageView marched, VkImageView distances,
                  const CloudsView& view) const noexcept {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing clouds without the air");
    HOG_ASSERT(view.weather.weather_before != VK_NULL_HANDLE, "drawing clouds without weather");
    const Push push{
        .eye_from_clip = eye_from_clip(view.camera, extent),
        .up_height = {Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
        .radii = {static_cast<float>(view.bend.radius), static_cast<float>(view.bend.faked),
                  static_cast<float>(view.base), static_cast<float>(view.weather.faded)},
        .veil_fog = {view.weather.falling ? 1.0F : 0.0F, static_cast<float>(view.weather.fog_top),
                     static_cast<float>(view.weather.highest_base), 0.0F}};
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
    push_sampled_image(commands, pipeline_, 9, marched, linear_.handle());
    push_sampled_image(commands, pipeline_, 10, distances, nearest_.handle());
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(commands, 3, 1, 0, 0);
}

} // namespace hog::render
