#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/bend.hpp>

#include <cstdint>

namespace hog::render {

class Device;

// Where the sea is seen from and what it reflects.
struct OceanView {
    Camera camera;
    DVec3 sun{1.0, 0.0, 0.0}; // unit, towards the sun
    Vec3 sky{0.35F, 0.55F, 0.85F};
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    terrain::Bend bend = {};
};

// The sea's surface at sea level, out to the horizon round the point under the eye. It reflects
// the sky and the sun over what's drawn beneath it, more the more glancing the view, and hides
// what's drawn after it below.
class Ocean {
public:
    [[nodiscard]] static Result<Ocean> create(const Device& device, VkFormat colour_format,
                                              VkFormat depth_format);

    // Records drawing into the colour and depth images being rendered to, of `extent`, over what's
    // already drawn there.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view) const;

private:
    Ocean() = default;

    Pipeline pipeline_;
    Buffer vertices_;
    Buffer indices_;
    std::uint32_t index_count_ = 0;
};

} // namespace hog::render
