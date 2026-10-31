#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/globe.hpp>
#include <hog/globe/material.hpp>

#include <cstdint>
#include <vector>

namespace hog::globe {

// A corner of a hexel's face, flat-lit by the face's normal.
struct HexelVertex {
    Vec3 position{0.0F}; // metres from the mesh's origin
    Vec3 normal{0.0F};   // unit, out of the face
    Material material = Material::Air;
};

// Faces as triangles, anticlockwise seen from outside.
struct HexelMesh {
    DVec3 origin{0.0};
    std::vector<HexelVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// The faces of the globe's hexels that face something clearer: air over water over everything
// else. The inside of the lowest shell never shows.
[[nodiscard]] HexelMesh mesh_hexels(const Globe& globe, DVec3 origin);

} // namespace hog::globe
