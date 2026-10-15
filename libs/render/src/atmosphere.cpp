#include <hog/render/atmosphere.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/shaders/air_tables.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace hog::render {

namespace {

constexpr VkFormat table_format = VK_FORMAT_R16G16B16A16_SFLOAT;

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr};
}

constexpr std::array table_bindings{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                    binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                                    binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};

std::array<float, 4> floats(DVec3 xyz, double w = 0.0) noexcept {
    return {static_cast<float>(xyz.x), static_cast<float>(xyz.y), static_cast<float>(xyz.z),
            static_cast<float>(w)};
}

Result<Image> make_table(const Device& device, int width, int height) {
    return make_image(device, {.format = table_format,
                               .extent = {.width = static_cast<std::uint32_t>(width),
                                          .height = static_cast<std::uint32_t>(height)},
                               .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT});
}

std::uint32_t groups(int across) noexcept {
    return (static_cast<std::uint32_t>(across) + 7) / 8;
}

} // namespace

AirConstants air_constants(const atmosphere::Air& air) noexcept {
    return {.radii = {static_cast<float>(air.bottom), static_cast<float>(air.top),
                      static_cast<float>(air.rayleigh_height), static_cast<float>(air.mie_height)},
            .scattering = floats(air.rayleigh_scattering, air.mie_scattering),
            .mie_ozone = {static_cast<float>(air.mie_extinction),
                          static_cast<float>(air.mie_asymmetry), static_cast<float>(air.ozone_peak),
                          static_cast<float>(air.ozone_half_width)},
            .ozone_absorption = floats(air.ozone_absorption),
            .ground_albedo = floats(air.ground_albedo)};
}

Result<Atmosphere> Atmosphere::create(Device& device, const atmosphere::Air& air) {
    Atmosphere made;
    made.air_ = air;
    const AirConstants constants = air_constants(air);
    auto buffer = make_filled_buffer(device, std::as_bytes(std::span{&constants, 1}),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!buffer) {
        return std::unexpected(std::move(buffer).error());
    }
    made.constants_ = std::move(buffer).value();
    auto transmittance =
        make_table(device, atmosphere::transmittance_width, atmosphere::transmittance_height);
    if (!transmittance) {
        return std::unexpected(std::move(transmittance).error());
    }
    made.transmittance_ = std::move(transmittance).value();
    auto scattering = make_table(device, atmosphere::scattering_size, atmosphere::scattering_size);
    if (!scattering) {
        return std::unexpected(std::move(scattering).error());
    }
    made.scattering_ = std::move(scattering).value();
    auto sampler = make_sampler(device, VK_FILTER_LINEAR);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    made.sampler_ = std::move(sampler).value();

    const auto transmitting = make_compute_pipeline(device, {.spirv = shaders::air_tables,
                                                             .entry = "transmittance_main",
                                                             .descriptors = table_bindings});
    if (!transmitting) {
        return std::unexpected(transmitting.error());
    }
    const auto scattering_pipeline = make_compute_pipeline(
        device,
        {.spirv = shaders::air_tables, .entry = "scattering_main", .descriptors = table_bindings});
    if (!scattering_pipeline) {
        return std::unexpected(scattering_pipeline.error());
    }

    const auto built = device.submit_now([&](VkCommandBuffer commands) {
        namespace use = image_use;
        VkImage transmittance_image = made.transmittance_.handle();
        VkImage scattering_image = made.scattering_.handle();
        transition(commands, transmittance_image, VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                   use::storage);
        transition(commands, scattering_image, VK_IMAGE_ASPECT_COLOR_BIT, use::discard,
                   use::storage);

        // The transmittance table is bound while building itself only to fill every binding.
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, transmitting->handle());
        push_uniform_buffer(commands, *transmitting, 0, made.constants());
        push_storage_image(commands, *transmitting, 1, made.transmittance_.view());
        push_sampled_image(commands, *transmitting, 2, made.scattering_.view(), made.sampler());
        vkCmdDispatch(commands, groups(atmosphere::transmittance_width),
                      groups(atmosphere::transmittance_height), 1);
        transition(commands, transmittance_image, VK_IMAGE_ASPECT_COLOR_BIT, use::storage,
                   use::sampled);

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, scattering_pipeline->handle());
        push_uniform_buffer(commands, *scattering_pipeline, 0, made.constants());
        push_storage_image(commands, *scattering_pipeline, 1, made.scattering_.view());
        push_sampled_image(commands, *scattering_pipeline, 2, made.transmittance_.view(),
                           made.sampler());
        vkCmdDispatch(commands, groups(atmosphere::scattering_size),
                      groups(atmosphere::scattering_size), 1);
        transition(commands, scattering_image, VK_IMAGE_ASPECT_COLOR_BIT, use::storage,
                   use::sampled);
    });
    if (!built) {
        return std::unexpected(built.error());
    }
    return made;
}

} // namespace hog::render
