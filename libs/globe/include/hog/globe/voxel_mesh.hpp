#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/globe/world.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace hog::globe {

// A corner of a voxel's face: metres from the mesh's origin, the face's outward normal, and what
// the voxel is made of.
struct VoxelVertex {
    Vec3 position{0.0F};
    Vec3 normal{0.0F};
    Material material = Material::Air;
    // A water sheet's corner on its edge, where it meets what holds no water as high: waves
    // mustn't lift it off what it meets.
    bool shore = false;
    // How a water sheet's water rises through the year, as voxels hold it, to be lifted by.
    std::uint8_t rise = 0;
    std::uint8_t peak = 0;
};

// The faces of voxels, two triangles each, anticlockwise seen from outside.
struct VoxelMesh {
    DVec3 origin{0.0}; // metres from the planet's centre
    std::vector<VoxelVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// The tile at a key of any level, where there's one.
using TileAt = std::function<const VoxelTree*(TileKey)>;

// The faces of the tile at `key`, its voxels `tile`, where its matter meets what isn't, each voxel
// a box of the planet's own shape. Shaped matter, or grain filling half a voxel, shows against
// anything else; a liquid only against open space. Past its sides the tiles on its face are read
// from `beside`, of its level or the finest coarser it has there; else finer, a voxel each read
// where it meets the tile; elsewhere the tile is taken to go on as its edge does. Given
// `shore_rise`, water's open top runs on over the shore beside it wherever the ground there is no
// more than that many metres over it, hidden under the ground at rest, for the sea to run up over.
[[nodiscard]] VoxelMesh mesh_tile(const Layers& layers, TileKey key, const VoxelTree& tile,
                                  const TileAt& beside = {},
                                  std::optional<double> shore_rise = std::nullopt);

} // namespace hog::globe
