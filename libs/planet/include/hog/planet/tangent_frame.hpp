#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>

namespace hog::planet {

// The planet spins about +z; north is towards +z.
inline constexpr DVec3 north_pole{0.0, 0.0, 1.0};

// Right-handed orthonormal frame on the surface: east x north = up, with up pointing away from
// the planet centre. At the poles, where east is undefined, east is taken as +x.
struct TangentFrame {
    DVec3 east{1.0, 0.0, 0.0};
    DVec3 north{0.0, 1.0, 0.0};
    DVec3 up{0.0, 0.0, 1.0};

    // Converts between (east, north, up) components and planet-centred axes.
    [[nodiscard]] DVec3 to_world(DVec3 local) const noexcept {
        return (east * local.x) + (north * local.y) + (up * local.z);
    }
    [[nodiscard]] DVec3 to_local(DVec3 world) const noexcept {
        return {glm::dot(world, east), glm::dot(world, north), glm::dot(world, up)};
    }
};

// Frame at a non-zero direction from the planet centre.
[[nodiscard]] TangentFrame tangent_frame(DVec3 direction);
// Frame at the cell's centre.
[[nodiscard]] TangentFrame tangent_frame(CellId cell);
[[nodiscard]] TangentFrame tangent_frame(WorldPosition position);

} // namespace hog::planet
