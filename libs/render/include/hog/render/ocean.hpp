#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sky.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>

#include <cstdint>

namespace hog::render {

class Device;

// Where the sea is seen from and what it reflects.
struct OceanView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0};      // unit, towards the sun
    Vec3 sky{0.35F, 0.55F, 0.85F}; // unless lit through the air
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
    // The frame's air, for the sea lit through it.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    AirBindings air = {};
};

// The sea's surface at sea level, out to the horizon round the point under the eye. It reflects
// the sky and the sun over what's drawn beneath it, more the more glancing the view, and hides
// what's drawn after it below.
class Ocean {
public:
    // Lit through the air, it reflects the sky as the eye sees it and the sun's light through the
    // air, is seen through the air and is tone mapped; otherwise it reflects a sky of one colour.
    [[nodiscard]] static Result<Ocean> create(const Device& device, VkFormat colour_format,
                                              VkFormat depth_format, bool air = false);

    // Records drawing into the colour and depth images being rendered to, of `extent`, over what's
    // already drawn there.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view) const;
    // Lit through the air and seen from above the sea, records the water beneath the surface as
    // the ground's shading has it where it lies too deep to show, behind all else: drawn before the
    // ground, so ground under the sea left undrawn looks as it would.
    void draw_deep(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view) const;

private:
    Ocean() = default;

    void record(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                const Pipeline& pipeline) const;

    Pipeline pipeline_;
    Pipeline deep_;
    Buffer vertices_;
    Buffer indices_;
    std::uint32_t index_count_ = 0;
    bool air_ = false;
};

} // namespace hog::render
