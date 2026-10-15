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
};

// The air is the planet's as it's drawn: Earth's scaled to the drawn radius. Metres as drawn times
// this are metres in the air's tables.
[[nodiscard]] double air_scale(const atmosphere::Air& air, const terrain::Bend& bend) noexcept;

// The sky from Hillaire's sky-view table, which is made afresh for each frame, and the sun's disc.
class Sky {
public:
    // `frames` in flight, each with its own table.
    [[nodiscard]] static Result<Sky> create(const Device& device, VkFormat colour_format,
                                            VkFormat depth_format, std::uint32_t frames = 1);

    // Records making `frame`'s table for the view, before rendering begins; it's then ready for
    // draw(). The frame's last submit must have finished.
    void record_table(VkCommandBuffer commands, std::uint32_t frame, const Atmosphere& atmosphere,
                      const SkyView& view) const;

    // Records drawing the sky over the whole of the colour image being rendered to, of `extent`,
    // behind anything drawn after it.
    void draw(VkCommandBuffer commands, VkExtent2D extent, std::uint32_t frame,
              const Atmosphere& atmosphere, const SkyView& view) const;

private:
    Sky() = default;

    Pipeline table_;
    Pipeline sky_;
    std::vector<Image> tables_;
};

} // namespace hog::render
