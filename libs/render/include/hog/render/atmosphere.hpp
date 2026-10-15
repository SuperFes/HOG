#pragma once

#include <hog/atmosphere/model.hpp>
#include <hog/core/result.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/vulkan.hpp>

#include <array>

namespace hog::render {

class Device;

// The air as the shaders' Air reads it, from a uniform buffer.
struct AirConstants {
    std::array<float, 4> radii{};
    std::array<float, 4> scattering{};
    std::array<float, 4> mie_ozone{};
    std::array<float, 4> ozone_absorption{};
    std::array<float, 4> ground_albedo{};
};

[[nodiscard]] AirConstants air_constants(const atmosphere::Air& air) noexcept;

// The air's transmittance and multiple scattering tabulated on the GPU, as atmosphere::Model
// tabulates them, ready to sample from any shader with the linear sampler.
class Atmosphere {
public:
    // Builds the tables, waiting for the GPU to finish.
    [[nodiscard]] static Result<Atmosphere> create(Device& device, const atmosphere::Air& air = {});

    [[nodiscard]] const atmosphere::Air& air() const noexcept { return air_; }
    [[nodiscard]] VkBuffer constants() const noexcept { return constants_.handle(); }
    [[nodiscard]] const Image& transmittance() const noexcept { return transmittance_; }
    [[nodiscard]] const Image& scattering() const noexcept { return scattering_; }
    [[nodiscard]] VkSampler sampler() const noexcept { return sampler_.handle(); }

private:
    Atmosphere() = default;

    atmosphere::Air air_;
    Buffer constants_;
    Image transmittance_;
    Image scattering_;
    Sampler sampler_;
};

} // namespace hog::render
