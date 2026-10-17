#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

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
    double faded = 1.0;   // from the weather map before, at 0, to the one after, at 1
    // Metres the clouds have drifted with the wind, in the planet's frame.
    DVec3 moved{0.0};
};

// `moved` within the clouds' period, so their shapes are the same and its precision kept.
[[nodiscard]] DVec3 within_period(DVec3 moved) noexcept;

// Under a layer of `cover`, the share of the sky it covers, holding `water` mm of cloud, as the
// layer is lit when drawn.
[[nodiscard]] CloudShade cloud_shade(double cover, double water) noexcept;

// Per metre, how much of the light along a view is lost to what's falling at `falling` mm an
// hour, `snow` of it as snow.
[[nodiscard]] double rain_extinction(double falling, double snow) noexcept;

// A layer of cloud round the planet, at a height above sea level, its cover and thickness from
// weather maps faded between, and its billows drifting with the wind, and what falls from it in
// veils to the ground. They're lit through the air and seen through it, and hidden by anything
// drawn nearer.
class Clouds {
public:
    // Weather maps are cubes of `map_size` texels a side, each texel as client::WeatherTexel.
    [[nodiscard]] static Result<Clouds> create(const Device& device, VkFormat colour_format,
                                               std::uint32_t map_size, std::uint32_t frames = 1);

    [[nodiscard]] std::size_t map_bytes() const noexcept;
    // Records copying in the maps faded from and to, faces in Vulkan's order, for `frame`, whose
    // last submit must have finished.
    Result<> update(VkCommandBuffer commands, std::uint32_t frame,
                    std::span<const std::byte> before, std::span<const std::byte> after);
    // Whether there are maps to draw from yet.
    [[nodiscard]] bool ready() const noexcept { return before_.ready() && after_.ready(); }

    // Records drawing the clouds over the colour image being rendered to, of `extent`, with no
    // depth image. `depth` is the picture's, ready to sample. Nothing's drawn until ready().
    void draw(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
              const CloudsView& view) const noexcept;

private:
    Clouds() = default;

    Pipeline pipeline_;
    Sampler nearest_;
    Sampler linear_;
    StreamedImage before_;
    StreamedImage after_;
    std::uint32_t map_size_ = 0;
    bool falling_ = false;
};

} // namespace hog::render
