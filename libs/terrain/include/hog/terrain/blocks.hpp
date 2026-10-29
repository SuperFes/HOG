#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/materials.hpp>
#include <hog/planet/volume.hpp>

#include <cstdint>
#include <vector>

namespace hog::terrain {

// A corner of a leaf's side, flat-lit by the side's normal.
struct BlockVertex {
    Vec3 position{0.0F}; // metres from the mesh's origin
    Vec3 normal{0.0F};   // unit, planet-centred, out of the side
    Vec3 colour{0.0F};   // linear

    friend bool operator==(const BlockVertex&, const BlockVertex&) = default;
};

// Sides of the volume's leaves as quads, two triangles of three indices each.
struct BlockMesh {
    WorldPosition origin;
    std::vector<BlockVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// How a material shows when drawn as blocks.
[[nodiscard]] Vec3 block_colour(planet::Material material);

// The sides showing in one region of the volume: each leaf's sides that face something clearer
// than it (air over water over everything else), laid from where the region's middle meets the
// planet's radius. A leaf a surface crosses shows as whatever is at its centre.
// Precondition: `region` is a column at the volume's region level.
[[nodiscard]] BlockMesh block_mesh(const planet::Volume& volume, double radius,
                                   planet::CellId region);

} // namespace hog::terrain
