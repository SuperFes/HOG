#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>

#include <cstdint>
#include <vector>

namespace hog::globe {

// A corner of a voxel's face: metres from the mesh's origin, the face's outward normal, and what
// the voxel is made of.
struct VoxelVertex {
    Vec3 position{0.0F};
    Vec3 normal{0.0F};
    Material material = Material::Air;
};

// The faces of voxels, two triangles each, anticlockwise seen from outside.
struct VoxelMesh {
    DVec3 origin{0.0}; // metres from the planet's centre
    std::vector<VoxelVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// The faces of the tile column at `corner` where its matter meets what isn't, each voxel a box of
// the planet's own shape `level` halvings coarser than the world's: 2^level cells across and
// layers tall. Shaped matter, or grain filling half a voxel, shows against anything else; a
// liquid only against open space. The columns beside it are read as the world holds them. From
// the column's floor, under which all is rock, up through its tiles; nothing where the world
// holds none of it.
[[nodiscard]] VoxelMesh mesh_tile_column(const World& world, Site corner, int level);

} // namespace hog::globe
