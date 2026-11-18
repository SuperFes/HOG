#pragma once

#include <hog/core/math.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace hog::client {

// A corner of the ground's surface: metres from the mesh's origin, the way it faces out of the
// solid, and the two materials it shows, as palette entries, with how much of it is the first.
// A normal of nothing is one not yet known.
struct ShapeVertex {
    Vec3 position{0.0F};
    Vec3 normal{0.0F};
    std::array<std::uint16_t, 2> materials{};
    float share = 1.0F;
};

// Triangles anticlockwise seen from the open side.
struct ShapeMesh {
    DVec3 origin{0.0}; // metres from the planet's centre
    std::vector<ShapeVertex> vertices;
    std::vector<std::uint32_t> indices;
};

} // namespace hog::client
