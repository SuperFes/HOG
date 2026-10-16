#pragma once

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>

#include <cstdint>
#include <vector>

namespace hog::render {

class Atmosphere;
class Device;

// Where the sky is seen from and lit by.
struct SkyView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0}; // unit, towards the sun
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    double exposure = 1.0; // how much brighter the picture is than the light reaching the eye
    // The sky's irradiance on level ground at the eye, as atmosphere::Model::sky_irradiance finds.
    DVec3 sky_light{0.0};
};

// What's drawn through the air reads it from these, all ready for any shader once a frame's
// record_frame() has been recorded.
struct AirBindings {
    VkBuffer air = VK_NULL_HANDLE;      // render::AirConstants
    VkBuffer lighting = VK_NULL_HANDLE; // the frame's Lighting, as atmosphere.slang has it
    VkImageView transmittance = VK_NULL_HANDLE;
    VkImageView sky_view = VK_NULL_HANDLE;
    VkImageView aerial = VK_NULL_HANDLE; // an array, a layer per slice of distance
    VkSampler sampler = VK_NULL_HANDLE;  // linear, clamped
};

// How far out of the air's metres aerial perspective is held: past the horizon from high over the
// islands.
inline constexpr double aerial_reach = 160'000.0;

// The air is the planet's as it's drawn: Earth's scaled to the drawn radius. Metres as drawn times
// this are metres in the air's tables.
[[nodiscard]] double air_scale(const atmosphere::Air& air, const terrain::Bend& bend) noexcept;

// The sky from Hillaire's sky-view table and the sun's disc, and the aerial perspective for what's
// drawn through the air, both made afresh for each frame.
class Sky {
public:
    // `frames` in flight, each with its own tables.
    [[nodiscard]] static Result<Sky> create(const Device& device, VkFormat colour_format,
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

    [[nodiscard]] AirBindings bindings(std::uint32_t frame,
                                       const Atmosphere& atmosphere) const noexcept;

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
};

} // namespace hog::render
