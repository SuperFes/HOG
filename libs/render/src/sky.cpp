#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/clouds.hpp>
#include <hog/render/device.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/shaders/aerial.hpp>
#include <hog/shaders/sky.hpp>
#include <hog/shaders/sky_view.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace hog::render {

namespace {

constexpr VkFormat table_format = VK_FORMAT_R16G16B16A16_SFLOAT;
// As atmosphere.slang's sky-view table.
constexpr std::uint32_t table_width = 192;
constexpr std::uint32_t table_height = 108;

// As atmosphere.slang's aerial-perspective volume, and its layer of the mist alone.
constexpr std::uint32_t aerial_size = 32;
constexpr std::uint32_t aerial_slices = 32;
constexpr std::uint32_t aerial_layers = aerial_slices + 1;

struct Lighting {
    Mat4 eye_from_clip;
    glm::vec4 up_height;
    glm::vec4 sun_exposure;
    glm::vec4 sky_scale;
    glm::vec4 reach;
    glm::vec4 weather;
    glm::vec4 shadow;
    glm::vec4 year;
};

static_assert(offsetof(Lighting, reach) + (3 * sizeof(float)) == sea_laying_at);

struct TablePush {
    glm::vec2 height_sun;
};

struct DrawPush {
    Mat4 eye_from_clip;
    glm::vec4 up_height;
    glm::vec4 sun_exposure;
};

static_assert(sizeof(DrawPush) <= 128);

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type,
                                               VkShaderStageFlags stages) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr};
}

constexpr std::array table_bindings{
    binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT)};

constexpr std::array aerial_bindings{
    binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)};

constexpr std::array sky_bindings{
    binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)};

void push_tables(VkCommandBuffer commands, const Pipeline& pipeline,
                 const Atmosphere& atmosphere) noexcept {
    push_uniform_buffer(commands, pipeline, 0, atmosphere.constants());
    push_sampled_image(commands, pipeline, 1, atmosphere.transmittance().view(),
                       atmosphere.sampler());
    push_sampled_image(commands, pipeline, 2, atmosphere.scattering().view(), atmosphere.sampler());
}

} // namespace

double air_scale(const atmosphere::Air& air, const terrain::Bend& bend) noexcept {
    return air.bottom / bend.faked;
}

Result<Sky> Sky::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                        std::uint32_t frames) {
    Sky sky;
    auto clear = WeatherMaps::create(device, 1);
    if (!clear) {
        return std::unexpected(std::move(clear).error());
    }
    sky.clear_bindings_ = clear->bindings(1.0);
    sky.clear_.emplace(std::move(clear).value());
    // Shading nothing: all the sun's light and the sky's straight through.
    constexpr std::array<std::byte, 4> unshadowed{std::byte{255}, std::byte{255}, std::byte{0},
                                                  std::byte{255}};
    auto filled =
        make_filled_image(device, {.format = VK_FORMAT_R8G8B8A8_UNORM}, std::span{unshadowed});
    if (!filled) {
        return std::unexpected(std::move(filled).error());
    }
    sky.unshadowed_ = std::move(filled).value();
    auto table = make_compute_pipeline(device, {.spirv = shaders::sky_view,
                                                .entry = "sky_view_main",
                                                .push_constant_bytes = sizeof(TablePush),
                                                .descriptors = table_bindings});
    if (!table) {
        return std::unexpected(std::move(table).error());
    }
    sky.table_ = std::move(table).value();
    auto aerial = make_compute_pipeline(
        device, {.spirv = shaders::aerial, .entry = "aerial_main", .descriptors = aerial_bindings});
    if (!aerial) {
        return std::unexpected(std::move(aerial).error());
    }
    sky.aerial_ = std::move(aerial).value();
    auto drawn = make_pipeline(device, {.spirv = shaders::sky,
                                        .colour_format = colour_format,
                                        .depth_format = depth_format,
                                        .depth_write = false,
                                        .push_constant_bytes = sizeof(DrawPush),
                                        .descriptors = sky_bindings});
    if (!drawn) {
        return std::unexpected(std::move(drawn).error());
    }
    sky.sky_ = std::move(drawn).value();
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        auto image =
            make_image(device, {.format = table_format,
                                .extent = {.width = table_width, .height = table_height},
                                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
        if (!image) {
            return std::unexpected(std::move(image).error());
        }
        sky.tables_.push_back(std::move(image).value());
        auto volume =
            make_image(device, {.format = table_format,
                                .extent = {.width = aerial_size, .height = aerial_size},
                                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .layers = aerial_layers});
        if (!volume) {
            return std::unexpected(std::move(volume).error());
        }
        sky.aerial_volumes_.push_back(std::move(volume).value());
        // Written by the sea too.
        auto lighting =
            make_buffer(device, sizeof(Lighting),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        Memory::Upload);
        if (!lighting) {
            return std::unexpected(std::move(lighting).error());
        }
        sky.lighting_.push_back(std::move(lighting).value());
    }
    return sky;
}

Result<> Sky::record_frame(VkCommandBuffer commands, VkExtent2D extent, std::uint32_t frame,
                           const Atmosphere& atmosphere, const SkyView& view) const {
    const Image& table = tables_.at(frame);
    const Image& volume = aerial_volumes_.at(frame);
    transition(commands, table.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::discard,
               image_use::storage);
    transition(commands, volume.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::discard,
               image_use::storage);
    if (auto recorded = record_tables(commands, extent, frame, atmosphere, view); !recorded) {
        return recorded;
    }
    transition(commands, table.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::storage,
               image_use::sampled);
    transition(commands, volume.handle(), VK_IMAGE_ASPECT_COLOR_BIT, image_use::storage,
               image_use::sampled);
    return {};
}

Result<> Sky::record_tables(VkCommandBuffer commands, VkExtent2D extent, std::uint32_t frame,
                            const Atmosphere& atmosphere, const SkyView& view) const {
    const double scale = air_scale(atmosphere.air(), view.bend);
    const double aspect =
        extent.height > 0 ? static_cast<double>(extent.width) / static_cast<double>(extent.height)
                          : 1.0;
    const Lighting lighting{
        .eye_from_clip = Mat4{glm::inverse(glm::dmat4{view_projection(view.camera, aspect)})},
        .up_height = {Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height * scale)},
        .sun_exposure = {Vec3{view.sun}, static_cast<float>(view.exposure)},
        .sky_scale = {Vec3{view.sky_light}, static_cast<float>(scale)},
        .reach = {static_cast<float>(aerial_reach), static_cast<float>(view.shade.direct),
                  static_cast<float>(view.shade.diffuse), 0.0F},
        .weather = {static_cast<float>(view.bend.radius), static_cast<float>(view.bend.faked),
                    static_cast<float>(view.weather.faded),
                    static_cast<float>(view.bend.eye_height)},
        .shadow = {view.cloud_shadow != VK_NULL_HANDLE
                       ? static_cast<float>(cloud_shadow_reach(view.bend.eye_height))
                       : 0.0F,
                   static_cast<float>(view.weather.lowest_base), static_cast<float>(view.mist),
                   static_cast<float>(view.haze)},
        .year = {view.daylight.hours, view.daylight.longest,
                 view.daylight.lengthening ? 1.0F : 0.0F, 0.0F}};
    const Buffer& buffer = lighting_.at(frame);
    std::memcpy(buffer.mapped().data(), &lighting, sizeof(lighting));
    if (auto flushed = buffer.flush(); !flushed) {
        return flushed;
    }

    const TablePush push{
        .height_sun = {lighting.up_height.w, static_cast<float>(glm::dot(view.sun, view.bend.up))}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, table_.handle());
    push_tables(commands, table_, atmosphere);
    push_storage_image(commands, table_, 3, tables_.at(frame).view());
    vkCmdPushConstants(commands, table_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                       &push);
    vkCmdDispatch(commands, (table_width + 7) / 8, (table_height + 7) / 8, 1);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, aerial_.handle());
    push_tables(commands, aerial_, atmosphere);
    push_uniform_buffer(commands, aerial_, 3, buffer.handle());
    push_storage_image(commands, aerial_, 4, aerial_volumes_.at(frame).view());
    const WeatherBindings weather =
        view.weather.sky_before != VK_NULL_HANDLE ? view.weather : clear_bindings_;
    push_sampled_image(commands, aerial_, 5, weather.sky_before, weather.sampler);
    push_sampled_image(commands, aerial_, 6, weather.sky_after, weather.sampler);
    push_sampled_image(commands, aerial_, 7,
                       view.cloud_shadow != VK_NULL_HANDLE ? view.cloud_shadow : unshadowed_.view(),
                       atmosphere.sampler());
    vkCmdDispatch(commands, aerial_size / 8, aerial_size / 8, aerial_layers);
    return {};
}

AirBindings Sky::bindings(std::uint32_t frame, const Atmosphere& atmosphere,
                          VkImageView cloud_shadow) const noexcept {
    return {.air = atmosphere.constants(),
            .lighting = lighting_[frame].handle(),
            .transmittance = atmosphere.transmittance().view(),
            .sky_view = tables_[frame].view(),
            .aerial = aerial_volumes_[frame].view(),
            .cloud_shadow = cloud_shadow != VK_NULL_HANDLE ? cloud_shadow : unshadowed_.view(),
            .sampler = atmosphere.sampler()};
}

void Sky::draw(VkCommandBuffer commands, VkExtent2D extent, std::uint32_t frame,
               const Atmosphere& atmosphere, const SkyView& view) const {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const DrawPush push{.eye_from_clip =
                            Mat4{glm::inverse(glm::dmat4{view_projection(view.camera, aspect)})},
                        .up_height = {Vec3{view.bend.up},
                                      static_cast<float>(view.bend.eye_height *
                                                         air_scale(atmosphere.air(), view.bend))},
                        .sun_exposure = {Vec3{view.sun}, static_cast<float>(view.exposure)}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_.handle());
    push_tables(commands, sky_, atmosphere);
    push_sampled_image(commands, sky_, 3, tables_.at(frame).view(), atmosphere.sampler());
    push_sampled_image(commands, sky_, 4, aerial_volumes_.at(frame).view(), atmosphere.sampler());
    vkCmdPushConstants(commands, sky_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(commands, 3, 1, 0, 0);
}

} // namespace hog::render
