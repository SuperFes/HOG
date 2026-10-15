#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/pipeline.hpp>
#include <hog/render/sampler.hpp>
#include <hog/render/vulkan.hpp>

namespace hog::render {

class Device;

// The sun's, as seen from Earth.
inline constexpr double sun_angular_radius = 0.004675; // radians

// A bright light, and the eye it's seen by.
struct GlareView {
    Camera camera;
    DVec3 towards{1.0, 0.0, 0.0}; // unit, towards the light
    double angular_radius = sun_angular_radius;
    // Reaching the eye, on a surface facing the light, in the sky's units.
    DVec3 irradiance{0.0};
    double exposure = 1.0; // as the rest of the picture is drawn with
};

// Of light from a point reaching the eye, the share per steradian scattered within the eye to
// `degrees` from it (Spencer et al. 1995, less the central peak, which falls within the disc).
[[nodiscard]] double eye_spread(double degrees) noexcept;
// Of that, the share in streaks: close in, and fading out within ten degrees or so.
[[nodiscard]] double eye_spread_streaked(double degrees) noexcept;

// How much brighter than its average the streaked glare is, at a `turn` (0 to 1, from the right
// round through down) about the light in the picture: fine streaks, averaging 1 round the turn.
[[nodiscard]] double streaks(double turn) noexcept;

// Light scattered within the eye round a bright light, as a glow fading over tens of degrees and
// streaked close in, laid over what's drawn. It's hidden as much as the light's disc is by anything
// drawn in front.
class Glare {
public:
    [[nodiscard]] static Result<Glare> create(const Device& device, VkFormat colour_format);

    // Records drawing the glare over the colour image being rendered to, of `extent`, with no
    // depth image. `depth` is the picture's, ready to sample.
    void draw(VkCommandBuffer commands, VkExtent2D extent, VkImageView depth,
              const GlareView& view) const noexcept;

private:
    Glare() = default;

    Pipeline pipeline_;
    Sampler sampler_;
};

} // namespace hog::render
