#pragma once

#include <hog/core/result.hpp>
#include <hog/render/vulkan.hpp>

namespace hog::render {

class Device;

// How shaders read an image: filtered linearly or taking the nearest texel, and clamped at its
// edges.
class Sampler {
public:
    Sampler() = default;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    Sampler(Sampler&& other) noexcept;
    Sampler& operator=(Sampler&& other) noexcept;
    ~Sampler();

    [[nodiscard]] VkSampler handle() const noexcept { return sampler_; }

private:
    friend Result<Sampler> make_sampler(const Device& device, VkFilter filter);

    void release() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

[[nodiscard]] Result<Sampler> make_sampler(const Device& device, VkFilter filter);

} // namespace hog::render
