#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>

#include <cstdint>

namespace hog::render {

class Device;

// Drops and flakes are drawn within this of the eye, repeating every twice it.
inline constexpr double precipitation_reach = 12.0; // metres
inline constexpr std::uint32_t most_precipitation = 16'384;
inline constexpr double rain_speed = 7.0; // m/s, as drops of 2 mm fall
inline constexpr double snow_speed = 1.0; // m/s

// How many drops and flakes are drawn round the eye for `falling` mm an hour, `snow` of it as
// snow, which falls as many more flakes than rain does drops.
[[nodiscard]] std::uint32_t precipitation_count(double falling, double snow) noexcept;
// Falling down `up` and blown along with `wind`, in m/s.
[[nodiscard]] DVec3 fall_velocity(DVec3 up, DVec3 wind, bool snow) noexcept;
// Where the eye is among particles that have moved with `velocity` for `seconds`, within the box
// they repeat in: each from 0 to twice precipitation_reach.
[[nodiscard]] DVec3 among(WorldPosition eye, DVec3 velocity, double seconds) noexcept;

// What falls round the eye, and where it's seen from.
struct PrecipitationView {
    Camera camera;
    // The frame's air, which lights it.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
    DVec3 up{0.0, 0.0, 1.0}; // unit, from the planet's centre through the eye
    DVec3 wind{0.0};         // m/s
    double falling = 0.0;    // mm an hour
    double snow = 0.0;       // the share of it falling as snow
    double seconds = 0.0;    // since any moment, so long as it's always the same one
};

// Rain as streaks and snow as flakes, falling round the eye with the wind, lit by the sun and sky
// and hidden by what's drawn nearer.
class Precipitation {
public:
    [[nodiscard]] static Result<Precipitation> create(const Device& device, VkFormat colour_format,
                                                      VkFormat depth_format);

    // Records drawing over the colour image being rendered to, of `extent`, tested against its
    // depth image but not writing it.
    void draw(VkCommandBuffer commands, VkExtent2D extent,
              const PrecipitationView& view) const noexcept;

private:
    Precipitation() = default;

    Pipeline pipeline_;
};

} // namespace hog::render
