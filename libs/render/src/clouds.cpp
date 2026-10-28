#include <hog/core/assert.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/clouds.hpp>
#include <hog/shaders/cloud_march.hpp>
#include <hog/shaders/cloud_resolve.hpp>
#include <hog/shaders/clouds.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>
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

struct ResolvePush {
    Mat4 eye_from_clip;
    Mat4 previous_clip_from_eye;
};

struct Push {
    Mat4 eye_from_clip;
    glm::vec4 up_height;
    glm::vec4 radii;
};

static_assert(sizeof(MarchPush) <= 128);
static_assert(sizeof(ResolvePush) <= 128);
static_assert(sizeof(Push) <= 128);

// As cloud_march.slang's.
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
// Metres the eye may move between frames and keep what was marched before, at least, and as a share
// of its height over the cloud.
constexpr double least_blended = 500.0;
constexpr double blended_share = 0.1;
// Metres the cloud's shadow reaches from the eye at least and at most.
constexpr double least_shadow_reach = 32'000.0;
constexpr double most_shadow_reach = 256'000.0;

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
                                 binding(1, sampled, fragment), binding(2, sampled, fragment),
                                 binding(3, sampled, fragment)};

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
                                       binding(12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, compute),
                                       binding(13, sampled, compute),
                                       binding(15, sampled, compute),
                                       binding(16, sampled, compute)};

constexpr std::array shadow_descriptors{binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, compute),
                                        binding(5, sampled, compute),
                                        binding(6, sampled, compute),
                                        binding(7, sampled, compute),
                                        binding(8, sampled, compute),
                                        binding(9, sampled, compute),
                                        binding(10, sampled, compute),
                                        binding(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, compute),
                                        binding(15, sampled, compute),
                                        binding(16, sampled, compute)};

constexpr std::array resolve_descriptors{binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, compute),
                                         binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, compute),
                                         binding(2, sampled, compute),
                                         binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, compute)};

double aspect_of(VkExtent2D extent) noexcept {
    return static_cast<double>(extent.width) / static_cast<double>(extent.height);
}

Mat4 eye_from_clip(const Camera& camera, VkExtent2D extent) noexcept {
    return Mat4{glm::inverse(glm::dmat4{view_projection(camera, aspect_of(extent))})};
}

Result<Image> make_shadow_map(const Device& device, CloudQuality quality) {
    const std::uint32_t size = shadow_size(quality);
    return make_image(device, {.format = Clouds::shadow_format,
                               .extent = {.width = size, .height = size},
                               // Read back to be looked at.
                               .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
}

} // namespace

std::string_view name_of(CloudQuality quality) noexcept {
    switch (quality) {
    case CloudQuality::Low:
        return "low";
    case CloudQuality::Medium:
        return "medium";
    case CloudQuality::High:
        break;
    }
    return "high";
}

std::optional<CloudQuality> cloud_quality_named(std::string_view name) noexcept {
    for (const CloudQuality quality :
         {CloudQuality::Low, CloudQuality::Medium, CloudQuality::High}) {
        if (name == name_of(quality)) {
            return quality;
        }
    }
    return std::nullopt;
}

CloudQuality next(CloudQuality quality) noexcept {
    switch (quality) {
    case CloudQuality::Low:
        return CloudQuality::Medium;
    case CloudQuality::Medium:
        return CloudQuality::High;
    case CloudQuality::High:
        break;
    }
    return CloudQuality::Low;
}

std::uint32_t march_scale(CloudQuality quality) noexcept {
    switch (quality) {
    case CloudQuality::Low:
        return 4;
    case CloudQuality::Medium:
        return 3;
    case CloudQuality::High:
        break;
    }
    return 2;
}

std::uint32_t shadow_size(CloudQuality quality) noexcept {
    return quality == CloudQuality::High ? 512 : 256;
}

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

double mist_through(double humidity, double fog_top, double height, double sun_mu) noexcept {
    return std::exp(-mist_above(humidity, fog_top, height) / std::max(sun_mu, least_sun));
}

double haze_through(double humidity, double fog_top, double height, double sun_mu) noexcept {
    return std::exp(-haze_above(humidity, fog_top, height) / std::max(sun_mu, least_sun));
}

CloudShade under_mist(CloudShade shade, double humidity, double fog_top, double height,
                      double sun_mu) noexcept {
    const double through = mist_through(humidity, fog_top, height, sun_mu);
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

double rain_beneath(double cover, double presence) noexcept {
    const double covered = std::clamp(cover, 0.0, 1.0);
    return std::lerp(1.0, std::clamp(presence, 0.0, 1.0) / std::max(covered, least_cover),
                     std::min(covered / least_cover, 1.0));
}

double furthest_blended(double eye_height, double highest_top) noexcept {
    return std::max(least_blended, blended_share * (eye_height - highest_top));
}

double cloud_shadow_reach(double eye_height) noexcept {
    double reach = least_shadow_reach;
    while (reach < 2.0 * eye_height && reach < most_shadow_reach) {
        reach *= 2.0;
    }
    return reach;
}

Result<Clouds> Clouds::create(Device& device, VkFormat colour_format, jobs::JobSystem& jobs,
                              CloudQuality quality) {
    Clouds clouds;
    clouds.device_ = &device;
    clouds.quality_ = quality;
    auto resolve = make_compute_pipeline(device, {.spirv = shaders::cloud_resolve,
                                                  .entry = "resolve_main",
                                                  .push_constant_bytes = sizeof(ResolvePush),
                                                  .descriptors = resolve_descriptors});
    if (!resolve) {
        return std::unexpected(std::move(resolve).error());
    }
    clouds.resolve_ = std::move(resolve).value();
    auto march = make_compute_pipeline(device, {.spirv = shaders::cloud_march,
                                                .entry = "march_main",
                                                .push_constant_bytes = sizeof(MarchPush),
                                                .descriptors = march_descriptors});
    if (!march) {
        return std::unexpected(std::move(march).error());
    }
    clouds.march_ = std::move(march).value();
    auto shadow = make_compute_pipeline(device, {.spirv = shaders::cloud_march,
                                                 .entry = "shadow_main",
                                                 .push_constant_bytes = sizeof(MarchPush),
                                                 .descriptors = shadow_descriptors});
    if (!shadow) {
        return std::unexpected(std::move(shadow).error());
    }
    clouds.shadow_ = std::move(shadow).value();
    auto shadow_map = make_shadow_map(device, quality);
    if (!shadow_map) {
        return std::unexpected(std::move(shadow_map).error());
    }
    clouds.shadow_map_ = std::move(shadow_map).value();
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

void Clouds::shadow(VkCommandBuffer commands, const CloudsView& view) const noexcept {
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "mapping the cloud's shadow without the air");
    HOG_ASSERT(view.weather.weather_before != VK_NULL_HANDLE,
               "mapping the cloud's shadow without weather");
    const MarchPush push{
        .eye_from_clip = Mat4{1.0F},
        .up_height = {Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
        .radii = {static_cast<float>(view.bend.radius), static_cast<float>(view.bend.faked),
                  static_cast<float>(view.weather.faded), 0.0F},
        .moved = {Vec3{within_period(view.moved)}, 0.0F},
        .bounds = {static_cast<float>(view.weather.lowest_base),
                   static_cast<float>(view.weather.highest_top), 0.0F, -1.0F}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, shadow_.handle());
    push_uniform_buffer(commands, shadow_, 1, view.air.lighting);
    push_sampled_image(commands, shadow_, 5, view.weather.weather_before, view.weather.sampler);
    push_sampled_image(commands, shadow_, 6, view.weather.weather_after, view.weather.sampler);
    push_sampled_image(commands, shadow_, 7, view.weather.sky_before, view.weather.sampler);
    push_sampled_image(commands, shadow_, 8, view.weather.sky_after, view.weather.sampler);
    push_sampled_image(commands, shadow_, 15, view.weather.drift_before, view.weather.sampler);
    push_sampled_image(commands, shadow_, 16, view.weather.drift_after, view.weather.sampler);
    push_sampled_image(commands, shadow_, 9, noise_.shape.view(), tiling_.handle());
    push_sampled_image(commands, shadow_, 10, noise_.detail.view(), tiling_.handle());
    push_storage_image(commands, shadow_, 14, shadow_map_.view());
    vkCmdPushConstants(commands, shadow_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                       &push);
    const std::uint32_t size = shadow_map_.desc().extent.width;
    vkCmdDispatch(commands, (size + 7) / 8, (size + 7) / 8, 1);
}

VkExtent2D Clouds::march_extent(VkExtent2D extent, CloudQuality quality) noexcept {
    const std::uint32_t scale = march_scale(quality);
    return {.width = (extent.width + scale - 1) / scale,
            .height = (extent.height + scale - 1) / scale};
}

Result<> Clouds::set_quality(CloudQuality quality) {
    if (quality == quality_) {
        return {};
    }
    if (shadow_size(quality) != shadow_size(quality_)) {
        auto shadow_map = make_shadow_map(*device_, quality);
        if (!shadow_map) {
            return std::unexpected(std::move(shadow_map).error());
        }
        shadow_map_ = std::move(shadow_map).value();
    }
    quality_ = quality;
    return {};
}

Result<> Clouds::march(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
                       VkImageView marched, VkImageView distances, const CloudsView& view) {
    if (extent.width == 0 || extent.height == 0) {
        return {};
    }
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "marching clouds without the air");
    HOG_ASSERT(view.weather.weather_before != VK_NULL_HANDLE, "marching clouds without weather");
    const VkExtent2D size = march_extent(extent);
    if (size.width != blend_extent_.width || size.height != blend_extent_.height) {
        for (Image& blend : blends_) {
            auto made = make_image(
                *device_, {.format = marched_format,
                           .extent = size,
                           .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
            if (!made) {
                return std::unexpected(std::move(made).error());
            }
            blend = std::move(made).value();
            transition(commands, blend.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::discard,
                       image_use::sampled);
        }
        blend_extent_ = size;
        frames_ = 0;
    }
    if (frames_ > 0 && glm::length(view.camera.eye - previous_eye_) >
                           furthest_blended(view.bend.eye_height, view.weather.highest_top)) {
        frames_ = 0;
    }

    // Fog's marched with the cloud, as cloud lying on the ground.
    const double lowest = view.weather.fog_top > 0.0 ? 0.0 : view.weather.lowest_base;
    const double highest = std::max(view.weather.highest_top, view.weather.fog_top);
    const std::uint32_t scale = march_scale(quality_);
    // Radians across a marched texel.
    const double texel = scale * 2.0 * std::tan(0.5 * view.camera.vertical_fov) / extent.height;
    const Mat4 from_clip = eye_from_clip(view.camera, extent);
    const MarchPush push{
        .eye_from_clip = from_clip,
        .up_height = {Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
        .radii = {static_cast<float>(view.bend.radius), static_cast<float>(view.bend.faked),
                  static_cast<float>(view.weather.faded), static_cast<float>(texel)},
        .moved = {Vec3{within_period(view.moved)}, static_cast<float>(scale)},
        .bounds = {static_cast<float>(lowest), static_cast<float>(highest),
                   static_cast<float>(frames_),
                   view.weather.falling ? static_cast<float>(view.weather.highest_base) : -1.0F}};
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
    push_sampled_image(commands, march_, 15, view.weather.drift_before, view.weather.sampler);
    push_sampled_image(commands, march_, 16, view.weather.drift_after, view.weather.sampler);
    push_sampled_image(commands, march_, 9, noise_.shape.view(), tiling_.handle());
    push_sampled_image(commands, march_, 10, noise_.detail.view(), tiling_.handle());
    push_storage_image(commands, march_, 11, marched);
    push_storage_image(commands, march_, 12, distances);
    push_sampled_image(commands, march_, 13, view.air.cloud_shadow, view.air.sampler);
    vkCmdPushConstants(commands, march_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                       &push);
    vkCmdDispatch(commands, (size.width + 7) / 8, (size.height + 7) / 8, 1);

    // What was just marched, read back by the blend; the marched images are the graph's, so a
    // barrier on all of memory rather than on them.
    const VkMemoryBarrier2 marched_to_read{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                           .pNext = nullptr,
                                           .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                           .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                           .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .pNext = nullptr,
                                      .dependencyFlags = 0,
                                      .memoryBarrierCount = 1,
                                      .pMemoryBarriers = &marched_to_read,
                                      .bufferMemoryBarrierCount = 0,
                                      .pBufferMemoryBarriers = nullptr,
                                      .imageMemoryBarrierCount = 0,
                                      .pImageMemoryBarriers = nullptr};
    vkCmdPipelineBarrier2(commands, &dependency);

    const std::uint32_t next = 1 - blended_;
    const Image& before = blends_.at(blended_);
    const Image& after = blends_.at(next);
    transition(commands, after.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::sampled,
               image_use::storage);
    // From this frame's eye to the last's clip space: moved by how far the eye moved.
    const DVec3 moved = view.camera.eye - previous_eye_;
    const ResolvePush resolve{.eye_from_clip = from_clip,
                              .previous_clip_from_eye =
                                  frames_ > 0 ? previous_clip_from_eye_ *
                                                    Mat4{glm::translate(glm::dmat4{1.0}, moved)}
                                              : Mat4{0.0F}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_.handle());
    push_storage_image(commands, resolve_, 0, marched);
    push_storage_image(commands, resolve_, 1, distances);
    push_sampled_image(commands, resolve_, 2, before.view(), linear_.handle());
    push_storage_image(commands, resolve_, 3, after.view());
    vkCmdPushConstants(commands, resolve_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(resolve),
                       &resolve);
    vkCmdDispatch(commands, (size.width + 7) / 8, (size.height + 7) / 8, 1);
    transition(commands, after.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::storage,
               image_use::sampled);

    blended_ = next;
    previous_clip_from_eye_ = view_projection(view.camera, aspect_of(extent));
    previous_eye_ = view.camera.eye;
    ++frames_;
    return {};
}

void Clouds::draw(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
                  VkImageView distances, const CloudsView& view) const noexcept {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing clouds without the air");
    HOG_ASSERT(blend_extent_.width > 0, "drawing clouds before marching them");
    const Push push{.eye_from_clip = eye_from_clip(view.camera, extent),
                    .up_height = {Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
                    .radii = {0.0F, static_cast<float>(view.bend.faked), 0.0F, 0.0F}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, view.air.lighting);
    push_sampled_image(commands, pipeline_, 1, depth, nearest_.handle());
    push_sampled_image(commands, pipeline_, 2, blends_.at(blended_).view(), linear_.handle());
    push_sampled_image(commands, pipeline_, 3, distances, nearest_.handle());
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(commands, 3, 1, 0, 0);
}

} // namespace hog::render
