#pragma once

#include <hog/core/math.hpp>

namespace hog::render {

// Where a view is from and which way it looks. The eye stays in doubles; the GPU only ever sees
// offsets from it.
struct Camera {
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition eye = {};
    DVec3 forward{0.0, 0.0, -1.0}; // unit length
    DVec3 up{0.0, 1.0, 0.0};       // unit length, at right angles to forward
    double vertical_fov = 1.0;     // radians
    double near_plane = 0.1;       // metres
};

// Turns offsets from the eye into view space: x right, y up, looking down -z.
[[nodiscard]] Mat4 view_rotation(const Camera& camera) noexcept;

// View space to Vulkan clip space with y down. Depth is reversed and has no far plane: 1 at the
// near plane, falling towards 0 at infinity, which spreads float precision evenly over distance.
[[nodiscard]] Mat4 projection(const Camera& camera, double aspect) noexcept;

// Offsets from the eye straight to clip space.
[[nodiscard]] Mat4 view_projection(const Camera& camera, double aspect) noexcept;

} // namespace hog::render
