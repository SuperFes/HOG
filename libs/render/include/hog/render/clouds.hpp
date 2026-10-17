#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/render/weather_maps.hpp>
#include <hog/terrain/bend.hpp>

namespace hog::render {

class Device;

// Metres across the clouds' largest billows; their shapes repeat every `cloud_period` metres.
inline constexpr double cloud_feature = 4000.0;
inline constexpr double cloud_period = 256.0 * cloud_feature;

// Where the clouds are seen from and lit by.
struct CloudsView {
    Camera camera;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // The frame's air, which lights them and lies between them and the eye.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
    double base = 2000.0; // metres above sea level the layer lies at
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WeatherBindings weather = {};
    // Metres the clouds have drifted with the wind, in the planet's frame.
    DVec3 moved{0.0};
};

// `moved` within the clouds' period, so their shapes are the same and its precision kept.
[[nodiscard]] DVec3 within_period(DVec3 moved) noexcept;

// Under a layer of `cover`, the share of the sky it covers, holding `water` mm of cloud, as the
// layer is lit when drawn.
[[nodiscard]] CloudShade cloud_shade(double cover, double water) noexcept;

// `shade` with what's over `height` metres above sea level of mist in air of `humidity` under fog
// to `fog_top`, with the sun `sun_mu` above the horizon: the mist takes from the sun's light, and
// scatters most of it on down.
[[nodiscard]] CloudShade under_mist(CloudShade shade, double humidity, double fog_top,
                                    double height, double sun_mu) noexcept;

// Per metre, how much of the light along a view is lost to what's falling at `falling` mm an
// hour, `snow` of it as snow.
[[nodiscard]] double rain_extinction(double falling, double snow) noexcept;

// A layer of cloud round the planet, at a height above sea level, its cover and thickness from
// weather maps faded between, and its billows drifting with the wind, and what falls from it in
// veils to the ground. They're lit through the air and seen through it, and hidden by anything
// drawn nearer.
class Clouds {
public:
    [[nodiscard]] static Result<Clouds> create(const Device& device, VkFormat colour_format);

    // Records drawing the clouds over the colour image being rendered to, of `extent`, with no
    // depth image. `depth` is the picture's, ready to sample.
    void draw(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
              const CloudsView& view) const noexcept;

private:
    Clouds() = default;

    Pipeline pipeline_;
    Sampler nearest_;
};

} // namespace hog::render
