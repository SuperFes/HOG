#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/cloud_noise.hpp>
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

// Where the clouds are seen from and lit by.
struct CloudsView {
    Camera camera;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // The frame's air, which lights them and lies between them and the eye.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
    double base = 2000.0; // metres above sea level of the cloud's base over the eye
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

// Clouds filling the sky where the weather has them, between the heights it has them at, their
// shapes from noise drifting with the wind; marched at half the picture's size, then drawn over
// it with what falls from them in veils to the ground and any fog lying low. All are lit through
// the air and seen through it, and hidden by anything drawn nearer.
class Clouds {
public:
    // The noise is baked on `jobs`.
    [[nodiscard]] static Result<Clouds> create(Device& device, VkFormat colour_format,
                                               jobs::JobSystem& jobs);

    // The size the clouds are marched at, for a picture of `extent`.
    [[nodiscard]] static VkExtent2D march_extent(VkExtent2D extent) noexcept;
    // Of the images the clouds are marched into.
    static constexpr VkFormat marched_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat distances_format = VK_FORMAT_R32_SFLOAT;
    // Metres the eye may move between frames and still have what was marched before blended in.
    static constexpr double furthest_blended = 500.0;

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
    Pipeline march_;
    Pipeline resolve_;
    Pipeline pipeline_;
    Sampler nearest_;
    Sampler linear_;
    Sampler tiling_;
    CloudNoise noise_;
    // Frames blended in turn: the last's, read, and the next's, written.
    std::array<Image, 2> blends_;
    std::uint32_t blended_ = 0; // which of blends_ was last written
    VkExtent2D blend_extent_{};
    std::uint32_t frames_ = 0; // since the eye last jumped
    Mat4 previous_clip_from_eye_{0.0F};
    WorldPosition previous_eye_;
};

} // namespace hog::render
