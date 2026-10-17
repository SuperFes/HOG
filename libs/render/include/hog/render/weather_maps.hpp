#pragma once

#include <hog/core/result.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/render/vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace hog::render {

class Device;

// What's drawn under the weather reads it from these cube maps, faded from those before to those
// after: the weather's, each texel as client::WeatherTexel, and the sky's, as client::SkyTexel.
struct WeatherBindings {
    VkImageView weather_before = VK_NULL_HANDLE;
    VkImageView weather_after = VK_NULL_HANDLE;
    VkImageView sky_before = VK_NULL_HANDLE;
    VkImageView sky_after = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE; // linear
    double faded = 1.0;                 // from the maps before, at 0, to those after, at 1
    bool falling = false;               // whether anything falls anywhere
    // Metres above sea level anywhere: the highest fog's top, 0 for none, and the lowest and
    // highest of the cloud's base and the highest of its top.
    double fog_top = 0.0;
    double lowest_base = 0.0;
    double highest_base = 0.0;
    double highest_top = 0.0;
};

// Per metre, how much of the light along a view the mist takes at `height` metres above sea level,
// or at sea level below it, in air of `humidity`, as client::SkyTexel holds it, under fog whose top
// is at `fog_top`.
[[nodiscard]] double mist_extinction(double humidity, double fog_top, double height) noexcept;
// Of light falling straight down from above the mist, how much the mist over `height` takes.
[[nodiscard]] double mist_above(double humidity, double fog_top, double height) noexcept;

// The weather's and the sky's cube maps, each before and after, as the server last sent them. Until
// they're first written they read as a clear, dry sky.
class WeatherMaps {
public:
    // Cubes of `map_size` texels a side.
    [[nodiscard]] static Result<WeatherMaps> create(Device& device, std::uint32_t map_size,
                                                    std::uint32_t frames = 1);

    // Of each map.
    [[nodiscard]] std::size_t map_bytes() const noexcept;
    // Records copying in the maps faded from and to, faces in Vulkan's order, for `frame`, whose
    // last submit must have finished.
    Result<> update(VkCommandBuffer commands, std::uint32_t frame,
                    std::span<const std::byte> weather_before,
                    std::span<const std::byte> weather_after, std::span<const std::byte> sky_before,
                    std::span<const std::byte> sky_after);
    [[nodiscard]] bool ready() const noexcept;

    [[nodiscard]] WeatherBindings bindings(double faded) const noexcept;

private:
    WeatherMaps() = default;

    Sampler linear_;
    StreamedImage weather_before_;
    StreamedImage weather_after_;
    StreamedImage sky_before_;
    StreamedImage sky_after_;
    Image clear_;
    std::uint32_t map_size_ = 0;
    bool falling_ = false;
    double fog_top_ = 0.0;
    double lowest_base_ = 0.0;
    double highest_base_ = 0.0;
    double highest_top_ = 0.0;
};

} // namespace hog::render
