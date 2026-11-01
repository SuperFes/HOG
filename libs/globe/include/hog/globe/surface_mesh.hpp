#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/world.hpp>

#include <cstdint>
#include <vector>

namespace hog::globe {

// Surfaces are drawn a chunk at a time: a square of columns this many levels coarser than the
// voxels', so 32 across.
inline constexpr int chunk_levels = 5;
inline constexpr int chunk_side = 1 << chunk_levels;

// Under the lowest ground in a chunk, caves are drawn this far down.
inline constexpr double surface_depth = 16.0; // metres

struct SurfaceVertex {
    Vec3 position{0.0F}; // metres from the mesh's origin
    Vec3 normal{0.0F};   // unit, out of the solid or the liquid
    Material material = Material::Air;

    friend bool operator==(const SurfaceVertex&, const SurfaceVertex&) = default;
};

// Where shaped matter meets open space, and where liquid meets air, as triangles anticlockwise
// seen from outside.
struct SurfaceMesh {
    DVec3 origin{0.0}; // metres from the planet's centre
    std::vector<SurfaceVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// The chunk a direction passes through: a cell chunk_levels coarser than the voxels'.
[[nodiscard]] Cell chunk_under(DVec3 direction, const Layers& layers);

// Chunks whose middles lie within `reach` metres of the unit direction `around`, along the
// planet's radius, nearest first.
[[nodiscard]] std::vector<Cell> chunks_near(const World& world, DVec3 around, double reach);

// Adds a chunk's surfaces by surface nets: a vertex in each cube of eight voxels the surface
// passes through, at the mean of where it crosses the cube's edges, and a quad across each edge
// it crosses. Each edge is the chunk's that holds its lower end, or, across the cube's edges, the
// lower face's, so chunks meet without gaps or overlaps. The surface is sought from surface_depth
// under the chunk's lowest ground to over its highest and around the sea's level, and through
// changes held next to those.
void add_chunk(SurfaceMesh& mesh, const World& world, Cell chunk);

} // namespace hog::globe
