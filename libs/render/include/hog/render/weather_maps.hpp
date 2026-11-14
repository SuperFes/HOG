#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/render/vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::render {

class Device;

// The most layers of cloud the air's held in: protocol::most_cloud_layers.
inline constexpr std::uint32_t most_cloud_layers = 24;

// How the air's layers lie, as protocol::AirLayers: `count` from `bottom` metres above sea level,
// the lowest `thickness` deep and each above `ratio` times the one below.
struct AirShape {
    std::uint32_t count = 0;
    double bottom = 0.0;
    double thickness = 0.0;
    double ratio = 1.0;
};

// What's drawn under the weather reads it from these cube maps, faded from those before to those
// after: the weather's, each texel as client::WeatherTexel, the sky's, as client::SkyTexel, and the
// drift's, as client::DriftTexel.
struct WeatherBindings {
    VkImageView weather_before = VK_NULL_HANDLE;
    VkImageView weather_after = VK_NULL_HANDLE;
    VkImageView sky_before = VK_NULL_HANDLE;
    VkImageView sky_after = VK_NULL_HANDLE;
    VkImageView drift_before = VK_NULL_HANDLE;
    VkImageView drift_after = VK_NULL_HANDLE;
    // Arrays of cubes of the cloud in each of the air's layers, bottom up, each texel its cover and
    // the square root of a sixteenth of its water's millimetres.
    VkImageView layers_before = VK_NULL_HANDLE;
    VkImageView layers_after = VK_NULL_HANDLE;
    AirShape air;
    VkSampler sampler = VK_NULL_HANDLE; // linear
    double faded = 1.0;                 // from the maps before, at 0, to those after, at 1
    bool falling = false;               // whether anything falls anywhere
    // Metres above sea level anywhere: the lowest and highest of the cloud's base and the highest
    // of its top.
    double lowest_base = 0.0;
    double highest_base = 0.0;
    double highest_top = 0.0;
    // Seconds since the maps faded from and to were taken: each place's cloud is read where its
    // drift brought it from.
    double age_before = 0.0;
    double age = 0.0;
    // Seconds the billows have flowed, within clouds.hpp's flow_period: each reads the drift where
    // it is, whoever looks.
    double flow = 0.0;
};

// Per metre, how much of the light along a view the mist takes at `height` metres above sea level,
// or at sea level below it, in air of `humidity`, as client::SkyTexel holds it. Cloud on the ground
// is marched as cloud.
[[nodiscard]] double mist_extinction(double humidity, double height) noexcept;
// Of light falling straight down from above the mist, how much the mist over `height` takes.
[[nodiscard]] double mist_above(double humidity, double height) noexcept;
// The weather's and the sky's cube maps, each before and after, as the server last sent them. Until
// they're first written they read as a clear, dry sky.
class WeatherMaps {
public:
    // Cubes of `map_size` texels a side.
    [[nodiscard]] static Result<WeatherMaps> create(Device& device, std::uint32_t map_size,
                                                    std::uint32_t frames = 1);

    // Of each of the weather's and the sky's maps, and of each of the drift's.
    [[nodiscard]] std::size_t map_bytes() const noexcept;
    [[nodiscard]] std::size_t drift_bytes() const noexcept;
    // Records copying in the maps faded from and to, faces in Vulkan's order, for `frame`, whose
    // last submit must have finished.
    struct Texels {
        std::span<const std::byte> weather_before;
        std::span<const std::byte> weather_after;
        std::span<const std::byte> sky_before;
        std::span<const std::byte> sky_after;
        std::span<const std::byte> drift_before;
        std::span<const std::byte> drift_after;
        // Each layer's cube in turn, up to most_cloud_layers of them; those not given are clear.
        std::span<const std::byte> layers_before;
        std::span<const std::byte> layers_after;
        AirShape air;
    };
    Result<> update(VkCommandBuffer commands, std::uint32_t frame, const Texels& texels);
    // Of each layer of the cloud's maps.
    [[nodiscard]] std::size_t layer_bytes() const noexcept;
    [[nodiscard]] bool ready() const noexcept;

    [[nodiscard]] WeatherBindings bindings(double faded, double age = 0.0,
                                           double age_before = 0.0) const noexcept;

private:
    WeatherMaps() = default;

    Sampler linear_;
    StreamedImage weather_before_;
    StreamedImage weather_after_;
    StreamedImage sky_before_;
    StreamedImage sky_after_;
    StreamedImage drift_before_;
    StreamedImage drift_after_;
    StreamedImage layers_before_;
    StreamedImage layers_after_;
    Image clear_;
    Image clear_layers_;
    AirShape air_;
    std::vector<std::byte> layered_; // the cloud's maps padded out to every layer
    std::uint32_t map_size_ = 0;
    // How far what each side's maps hold reaches: whether anything falls, and the metres above sea
    // level of the cloud's lowest and highest base and highest top.
    struct Reach {
        bool falling = false;
        double lowest_base = 0.0;
        double highest_base = 0.0;
        double highest_top = 0.0;
    };
    Reach before_;
    Reach after_;
};

} // namespace hog::render
