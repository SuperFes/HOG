#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/cloud_noise.hpp>
#include <hog/render/cloud_quality.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/render/weather_maps.hpp>
#include <hog/terrain/bend.hpp>

#include <array>
#include <cstdint>

namespace hog::render {

class Device;

// Metres over which the clouds' shapes repeat: a whole number of their noise's volumes.
inline constexpr double cloud_period = 1'024'000.0;

[[nodiscard]] std::uint32_t march_scale(CloudQuality quality) noexcept;
[[nodiscard]] std::uint32_t shadow_size(CloudQuality quality) noexcept;

// Where the clouds are seen from and lit by.
struct CloudsView {
    Camera camera;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // The frame's air, which lights them and lies between them and the eye.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
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

// Of light falling straight down onto mist in air of `humidity` under fog to `fog_top`, from the
// sun `sun_mu` above the horizon, the share let through to `height` metres above sea level.
[[nodiscard]] double mist_through(double humidity, double fog_top, double height,
                                  double sun_mu) noexcept;
// `shade` with what's over `height` metres above sea level of mist in air of `humidity` under fog
// to `fog_top`, with the sun `sun_mu` above the horizon: the mist takes from the sun's light, and
// scatters most of it on down.
[[nodiscard]] CloudShade under_mist(CloudShade shade, double humidity, double fog_top,
                                    double height, double sun_mu) noexcept;

// Per metre, how much of the light along a view is lost to what's falling at `falling` mm an
// hour, `snow` of it as snow.
[[nodiscard]] double rain_extinction(double falling, double snow) noexcept;
// Of what the weather has falling over a place, `cover` of whose sky is covered, the share falling
// where the cloud above is there by `presence`, from 0 to 1: all of it beneath the cloud, none in
// the gaps, and as much as the weather has over the place; evenly where there's too little cover
// to follow.
[[nodiscard]] double rain_beneath(double cover, double presence) noexcept;

// Metres the map of the cloud's shadow reaches from an eye `eye_height` metres above sea level,
// east, west, north and south: further the higher it is, to cover the ground it sees, in steps that
// double, so the map's texels keep their size between them.
[[nodiscard]] double cloud_shadow_reach(double eye_height) noexcept;

// Clouds filling the sky where the weather has them, between the heights it has them at, their
// shapes from noise drifting with the wind, and what falls from them to the ground; marched at a
// share of the picture's size, as their quality has it, then drawn over it with any fog lying low.
// All are lit through the air and seen through it, and hidden by anything drawn nearer. Their
// shadow's mapped round the eye, for all that's lit under them.
class Clouds {
public:
    // The noise is baked on `jobs`.
    [[nodiscard]] static Result<Clouds> create(Device& device, VkFormat colour_format,
                                               jobs::JobSystem& jobs,
                                               CloudQuality quality = CloudQuality::High);

    // The size the clouds are marched at, for a picture of `extent`.
    [[nodiscard]] static VkExtent2D march_extent(VkExtent2D extent, CloudQuality quality) noexcept;
    [[nodiscard]] VkExtent2D march_extent(VkExtent2D extent) const noexcept {
        return march_extent(extent, quality_);
    }
    [[nodiscard]] CloudQuality quality() const noexcept { return quality_; }
    // Remakes what's drawn with that differs between qualities, so nothing the GPU's still using
    // may hold it.
    Result<> set_quality(CloudQuality quality);
    // Of the images the clouds are marched into.
    static constexpr VkFormat marched_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat distances_format = VK_FORMAT_R32_SFLOAT;
    // Metres the eye may move between frames and still have what was marched before blended in.
    static constexpr double furthest_blended = 500.0;
    // Of the map of the cloud's shadow, as cloud_march.slang's shadow_main() leaves it.
    static constexpr VkFormat shadow_format = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Records mapping the cloud's shadow round the eye into shadow_image(), a storage image, as
    // far as the Lighting of `view.air` reaches, which must be written before this is submitted.
    void shadow(VkCommandBuffer commands, const CloudsView& view) const noexcept;
    [[nodiscard]] const Image& shadow_image() const noexcept { return shadow_map_; }

    // Records marching the clouds for a picture of `extent` into `marched` and `distances`, storage
    // images of march_extent(), then blending them with what was marched for the frames before, as
    // draw() draws them. `depth` is the picture's, ready to sample. Frames are blended while the
    // picture keeps its size and the eye doesn't jump.
    Result<> march(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
                   VkImageView marched, VkImageView distances, const CloudsView& view);
    // Records drawing them over the colour image being rendered to, of `extent`, with no depth
    // image, from the last march() and its `distances`, ready to sample.
    void draw(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth, VkImageView distances,
              const CloudsView& view) const noexcept;

private:
    Clouds() = default;

    const Device* device_ = nullptr;
    CloudQuality quality_ = CloudQuality::High;
    Pipeline march_;
    Pipeline shadow_;
    Pipeline resolve_;
    Pipeline pipeline_;
    Sampler nearest_;
    Sampler linear_;
    Sampler tiling_;
    CloudNoise noise_;
    Image shadow_map_;
    // Frames blended in turn: the last's, read, and the next's, written.
    std::array<Image, 2> blends_;
    std::uint32_t blended_ = 0; // which of blends_ was last written
    VkExtent2D blend_extent_{};
    std::uint32_t frames_ = 0; // since the eye last jumped
    Mat4 previous_clip_from_eye_{0.0F};
    WorldPosition previous_eye_;
};

} // namespace hog::render
