#include <hog/render/device.hpp>
#include <hog/render/sampler.hpp>

#include <algorithm>
#include <utility>

namespace hog::render {

Sampler::Sampler(Sampler&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      sampler_(std::exchange(other.sampler_, VK_NULL_HANDLE)) {}

Sampler& Sampler::operator=(Sampler&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        sampler_ = std::exchange(other.sampler_, VK_NULL_HANDLE);
    }
    return *this;
}

Sampler::~Sampler() {
    release();
}

void Sampler::release() noexcept {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
}

Result<Sampler> Sampler::create(const Device& device, const VkSamplerCreateInfo& info) {
    Sampler sampler;
    if (const VkResult made = vkCreateSampler(device.handle(), &info, nullptr, &sampler.sampler_);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating a sampler");
    }
    sampler.device_ = device.handle();
    return sampler;
}

Result<Sampler> make_sampler(const Device& device, VkFilter filter) {
    return Sampler::create(device, {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                    .pNext = nullptr,
                                    .flags = 0,
                                    .magFilter = filter,
                                    .minFilter = filter,
                                    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .mipLodBias = 0.0F,
                                    .anisotropyEnable = VK_FALSE,
                                    .maxAnisotropy = 1.0F,
                                    .compareEnable = VK_FALSE,
                                    .compareOp = VK_COMPARE_OP_ALWAYS,
                                    .minLod = 0.0F,
                                    .maxLod = 0.0F,
                                    .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                                    .unnormalizedCoordinates = VK_FALSE});
}

Result<Sampler> make_tiling_sampler(const Device& device) {
    // Beyond 16, sharper ground at a glance costs more than it shows.
    const float anisotropy = std::min(device.max_anisotropy(), 16.0F);
    return Sampler::create(device, {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                    .pNext = nullptr,
                                    .flags = 0,
                                    .magFilter = VK_FILTER_LINEAR,
                                    .minFilter = VK_FILTER_LINEAR,
                                    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                    .mipLodBias = 0.0F,
                                    .anisotropyEnable = anisotropy > 1.0F ? VK_TRUE : VK_FALSE,
                                    .maxAnisotropy = anisotropy,
                                    .compareEnable = VK_FALSE,
                                    .compareOp = VK_COMPARE_OP_ALWAYS,
                                    .minLod = 0.0F,
                                    .maxLod = VK_LOD_CLAMP_NONE,
                                    .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                                    .unnormalizedCoordinates = VK_FALSE});
}

} // namespace hog::render
