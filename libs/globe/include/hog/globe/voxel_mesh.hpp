#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/globe/world.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace hog::globe {

inline constexpr float no_edge = std::numeric_limits<float>::infinity();

// A corner of a voxel's face: metres from the mesh's origin, the face's outward normal, and what
// the voxel is made of.
struct VoxelVertex {
    Vec3 position{0.0F};
    Vec3 normal{0.0F};
    Material material = Material::Air;
    // Of a water sheet's corner, metres along the ground from the sheet's edge, where it meets
    // what holds no water as high: waves rise from nothing there. `no_edge` where none's near.
    float from_edge = no_edge;
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

// The tile at a key of any level, where there's one.
using TileAt = std::function<const VoxelTree*(TileKey)>;

// The faces of the tile at `key`, its voxels `tile`, drawn as mesh_tile_column draws a column's.
// Past its sides the tiles on its face are read from `beside`, of its level or the finest coarser
// it has there; else finer, a voxel each read where it meets the tile; elsewhere the tile is taken
// to go on as its edge does.
[[nodiscard]] VoxelMesh mesh_tile(const Layers& layers, TileKey key, const VoxelTree& tile,
                                  const TileAt& beside = {});

} // namespace hog::globe
