#include <hog/core/assert.hpp>
#include <hog/render/precipitation.hpp>
#include <hog/shaders/precipitation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace hog::render {

namespace {

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 rain_shift_box;
    glm::vec4 snow_shift_share;
    glm::vec4 rain_velocity_pixel;
    glm::vec4 snow_velocity_time;
};

static_assert(sizeof(Push) <= 128);

constexpr double box = 2.0 * precipitation_reach;
// Drops an hour's mm brings within the box, about as many as look like rain that heavy.
constexpr double drops_per_mm = 1200.0;
constexpr double flakes_per_drop = 5.0;
// Flakes sway to and fro over this.
constexpr double sway_period = 2.0; // seconds

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = nullptr};
}

constexpr std::array descriptors{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                 binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                 binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};

} // namespace

std::uint32_t precipitation_count(double falling, double snow) noexcept {
    const double share = std::clamp(snow, 0.0, 1.0);
    const double count =
        std::max(falling, 0.0) * drops_per_mm * (1.0 + (share * (flakes_per_drop - 1.0)));
    return static_cast<std::uint32_t>(
        std::min(std::round(count), static_cast<double>(most_precipitation)));
}

DVec3 fall_velocity(DVec3 up, DVec3 wind, bool snow) noexcept {
    const DVec3 blown = wind - (up * glm::dot(wind, up));
    return blown - (up * (snow ? snow_speed : rain_speed));
}

DVec3 among(WorldPosition eye, DVec3 velocity, double seconds) noexcept {
    const DVec3 at = eye.metres() - (velocity * seconds);
    return at - (box * glm::floor(at / box));
}

Result<Precipitation> Precipitation::create(const Device& device, VkFormat colour_format,
                                            VkFormat depth_format) {
    Precipitation precipitation;
    auto pipeline = make_pipeline(device, {.spirv = shaders::precipitation,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .depth_write = false,
                                           .blend = Blend::Premultiplied,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = descriptors});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    precipitation.pipeline_ = std::move(pipeline).value();
    return precipitation;
}

void Precipitation::draw(VkCommandBuffer commands, VkExtent2D extent,
                         const PrecipitationView& view) const noexcept {
    const std::uint32_t count = precipitation_count(view.falling, view.snow);
    if (extent.width == 0 || extent.height == 0 || count == 0) {
        return;
    }
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing rain and snow without the air");
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const double pixel = 2.0 * std::tan(0.5 * view.camera.vertical_fov) / extent.height;
    const DVec3 rain = fall_velocity(view.up, view.wind, false);
    const DVec3 snow = fall_velocity(view.up, view.wind, true);
    const Push push{.clip_from_eye = view_projection(view.camera, aspect),
                    .rain_shift_box = {Vec3{among(view.camera.eye, rain, view.seconds)},
                                       static_cast<float>(box)},
                    .snow_shift_share = {Vec3{among(view.camera.eye, snow, view.seconds)},
                                         static_cast<float>(std::clamp(view.snow, 0.0, 1.0))},
                    .rain_velocity_pixel = {Vec3{rain}, static_cast<float>(pixel)},
                    .snow_velocity_time = {
                        Vec3{snow}, static_cast<float>(std::fmod(view.seconds, sway_period))}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, view.air.air);
    push_uniform_buffer(commands, pipeline_, 1, view.air.lighting);
    push_sampled_image(commands, pipeline_, 2, view.air.transmittance, view.air.sampler);
    vkCmdPushConstants(commands, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(commands, 6, count, 0, 0);
}

} // namespace hog::render
