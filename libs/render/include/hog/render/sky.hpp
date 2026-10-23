#pragma once

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/flora/season.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/render/weather_maps.hpp>
#include <hog/terrain/bend.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace hog::render {

class Atmosphere;
class Device;

// Of light falling on a layer of cloud, the shares that reach the ground below it.
struct CloudShade {
    double direct = 1.0;  // straight through the gaps and thin cloud, as from the sun
    double diffuse = 0.0; // scattered through, lighting the ground as the sky does
};

// Where the sky is seen from and lit by.
struct SkyView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0}; // unit, towards the sun
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    double exposure = 1.0; // how much brighter the picture is than the light reaching the eye
    // The sky's irradiance on level ground at the eye, as atmosphere::Model::sky_irradiance finds.
    DVec3 sky_light{0.0};
    // What any cloud over the eye lets through to what's drawn under it, and the air between, and
    // any mist over it as well.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    CloudShade shade = {};
    // The mist's from these; a clear, dry sky with none.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WeatherBindings weather = {};
    // The cloud's shadow round the eye, as Clouds::shadow() makes it, ready to sample, which
    // shades what's drawn in its reach in place of `shade`; none for no shadow.
    VkImageView cloud_shadow = VK_NULL_HANDLE;
    // Of light falling straight down, the share the mist over the eye lets through; and the haze
    // alone, less any fog.
    double mist = 1.0;
    double haze = 1.0;
    // The year at the eye, which the leaves drawn keep.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    flora::Daylight daylight = {};
    // Metres the gusts' field has drifted, in the planet's frame.
    DVec3 gusts_drifted{0.0};
    double gusts_wind = 0.0;  // m/s the wind blows near the ground on the whole
    double swaying_for = 0.0; // seconds the grass has swayed for, as GrassView's
};

// Where in the frame's Lighting, in bytes, the sea writes whether it lays the water over what's
// drawn beneath it, as atmosphere.slang's reach.w.
inline constexpr std::uint32_t sea_laying_at = 124;
// Where the sea writes the plane of its surface at the eye, as atmosphere.slang's water.
inline constexpr std::uint32_t sea_water_at = 176;

// What's drawn through the air reads it from these, all ready for any shader once a frame's
// record_frame() has been recorded.
struct AirBindings {
    VkBuffer air = VK_NULL_HANDLE;      // render::AirConstants
    VkBuffer lighting = VK_NULL_HANDLE; // the frame's Lighting, as atmosphere.slang has it
    VkImageView transmittance = VK_NULL_HANDLE;
    VkImageView sky_view = VK_NULL_HANDLE;
    // An array, a layer per slice of distance, and one more of the mist alone to the sky.
    VkImageView aerial = VK_NULL_HANDLE;
    // As SkyView's, or an image shading nothing when there's none.
    VkImageView cloud_shadow = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE; // linear, clamped
};

// How far out of the air's metres aerial perspective is held: past the horizon from high over the
// islands.
inline constexpr double aerial_reach = 160'000.0;

// The air is the planet's as it's drawn: Earth's scaled to the drawn radius. Metres as drawn times
// this are metres in the air's tables.
[[nodiscard]] double air_scale(const atmosphere::Air& air, const terrain::Bend& bend) noexcept;

// The sky from Hillaire's sky-view table and the sun's disc, and the aerial perspective for what's
// drawn through the air and any mist, both made afresh for each frame.
class Sky {
public:
    // `frames` in flight, each with its own tables.
    [[nodiscard]] static Result<Sky> create(Device& device, VkFormat colour_format,
                                            VkFormat depth_format, std::uint32_t frames = 1);

    // Records making `frame`'s tables for the view, of `extent`, before rendering begins. The
    // frame's last submit must have finished.
    Result<> record_frame(VkCommandBuffer commands, VkExtent2D extent, std::uint32_t frame,
                          const Atmosphere& atmosphere, const SkyView& view) const;
    // As record_frame(), with `frame`'s table and aerial volume already storage images, and left
    // as they're written.
    Result<> record_tables(VkCommandBuffer commands, VkExtent2D extent, std::uint32_t frame,
                           const Atmosphere& atmosphere, const SkyView& view) const;
    [[nodiscard]] const Image& table(std::uint32_t frame) const { return tables_.at(frame); }
    [[nodiscard]] const Image& aerial_volume(std::uint32_t frame) const {
        return aerial_volumes_.at(frame);
    }

    // With `cloud_shadow` as SkyView's.
    [[nodiscard]] AirBindings bindings(std::uint32_t frame, const Atmosphere& atmosphere,
                                       VkImageView cloud_shadow = VK_NULL_HANDLE) const noexcept;

    // Records drawing the sky over the whole of the colour image being rendered to, of `extent`,
    // behind anything drawn after it.
    void draw(VkCommandBuffer commands, VkExtent2D extent, std::uint32_t frame,
              const Atmosphere& atmosphere, const SkyView& view) const;

private:
    Sky() = default;

    Pipeline table_;
    Pipeline aerial_;
    Pipeline sky_;
    std::vector<Image> tables_;
    std::vector<Image> aerial_volumes_;
    std::vector<Buffer> lighting_;
    // A clear, dry sky's, for views with no weather.
    std::optional<WeatherMaps> clear_;
    WeatherBindings clear_bindings_;
    // For views with no cloud's shadow.
    Image unshadowed_;
};

} // namespace hog::render
