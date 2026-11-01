#pragma once

#include <hog/core/math.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace hog::terrain {

// A corner of a surface, textured by the tiles of the triangle it's drawn in.
struct BlockVertex {
    Vec3 position{0.0F}; // metres from the mesh's origin
    Vec3 normal{0.0F};   // unit, planet-centred, out of the side
    // The tiles at the triangle's three corners, and which of them this corner is. A vertex shared
    // between triangles names its own tile three times, so whichever triangle draws it finds it.
    std::array<std::uint8_t, 3> tiles{};
    std::uint8_t corner = 0;

    friend bool operator==(const BlockVertex&, const BlockVertex&) = default;
};

// Triangles of three indices each, anticlockwise seen from outside.
struct BlockMesh {
    WorldPosition origin;
    std::vector<BlockVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// Gives each triangle whose corners wear different tiles three corners of its own, naming all
// three tiles, so they blend across it. Every vertex must name one tile three times.
void mix_corners(BlockMesh& mesh);

} // namespace hog::terrain
