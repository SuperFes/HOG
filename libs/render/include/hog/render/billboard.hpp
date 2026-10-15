#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/render/vulkan.hpp>

#include <cstddef>
#include <span>

namespace hog::render {

class Device;

// Where a billboard stands: on its feet, upright along `up`, and how tall.
struct Standing {
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition feet = {};
    DVec3 up{0.0, 0.0, 1.0}; // unit
    double height = 1.0;     // metres
};

// A picture standing upright in the world, turned about its up to face the eye, unlit. Texels
// less than half opaque are see-through, so it hides what's behind it by depth.
class Billboard {
public:
    // From sRGB texels with alpha, 4 bytes each, rows packed tightly from the top.
    [[nodiscard]] static Result<Billboard> create(Device& device, VkFormat colour_format,
                                                  VkFormat depth_format, VkExtent2D size,
                                                  std::span<const std::byte> texels);

    // Records drawing into the colour and depth images being rendered to, of `extent`, as wide
    // as the picture is for its height.
    void draw(VkCommandBuffer commands, VkExtent2D extent, const Camera& camera,
              const Standing& standing) const noexcept;

private:
    Billboard() = default;

    Pipeline pipeline_;
    Sampler sampler_;
    StreamedImage picture_;
    double aspect_ = 1.0; // width over height
};

} // namespace hog::render
