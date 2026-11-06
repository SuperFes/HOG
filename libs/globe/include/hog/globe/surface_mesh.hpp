#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/detail.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/world.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace hog::globe {

// Surfaces are drawn a chunk at a time: a square of columns this many levels coarser than the
// voxels', so 32 across.
inline constexpr int chunk_levels = 5;
inline constexpr int chunk_side = 1 << chunk_levels;

// The ground is drawn from this far under the lowest in a chunk.
inline constexpr double surface_depth = 16.0; // metres

struct SurfaceVertex {
    Vec3 position{0.0F}; // metres from the mesh's origin
    Vec3 normal{0.0F};   // unit, out of the solid or the liquid
    Material material = Material::Air;
    float wet = 0.0F; // of the ground's pores there, the share holding liquid
    float glow = 0.0F; // from 0 to 1, how hot it shines: molten or red-hot rock

    friend bool operator==(const SurfaceVertex&, const SurfaceVertex&) = default;
};

// Where shaped matter meets open space, and where liquid meets air, as triangles anticlockwise
// seen from outside.
struct SurfaceMesh {
    DVec3 origin{0.0}; // metres from the planet's centre
    std::vector<SurfaceVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// A chunk at detail n is a cell chunk_levels + n coarser than the voxels', drawn from every 2^n-th
// voxel along each axis, so still 32 across; each level's voxels are among the next finer's.
[[nodiscard]] int detail_of(Cell chunk, const Layers& layers);
[[nodiscard]] constexpr int apart_at(int detail) noexcept {
    return 1 << detail;
}

// The chunk a direction passes through, at a detail.
[[nodiscard]] Cell chunk_under(DVec3 direction, const Layers& layers, int detail = 0);

// Chunks whose middles lie within `reach` metres of the unit direction `around`, along the
// planet's radius, nearest first.
[[nodiscard]] std::vector<Cell> chunks_near(const World& world, DVec3 around, double reach);

// Adds a chunk's surfaces by dual contouring: a vertex in each cube of eight voxels the surface
// passes through, where the planes tangent to it at its crossings of the cube's edges meet, and a
// quad across each edge it crosses. Each edge is the chunk's that holds its lower end, or, across
// the cube's edges, the lower face's, so chunks of a detail meet without gaps or overlaps. The
// surface is sought from surface_depth under the chunk's lowest ground to over its highest,
// around the sea's level, round every cave under the chunk, found by casting down its columns,
// and through changes held next to those. Coarser chunks are generated as much coarser.
// Where the chunks beside it are drawn at other details, its surfaces meet theirs: the boxes of
// voxels either side meet, and the vertex of a coarser box takes in where the surface crosses
// the finer boxes' edges on its faces.
// What lies under the ground can be seen only from inside it: with a `sight`, caves are cast for
// only between its radii, so far more of the chunk isn't generated than is. Without, all are.
struct Sight {
    double lowest = 0.0;
    double highest = 0.0;
};
// A sight is kept while the eye stays within a band of radii this tall, round which it reaches
// `sight_reach` over and under, so a chunk is meshed again only as the eye leaves its band.
inline constexpr double sight_step = 32.0;  // metres
inline constexpr double sight_reach = 64.0; // metres
// The band of radii a point lies in, numbered from 1; 0 is no sight at all.
[[nodiscard]] std::uint64_t sight_band(double radius) noexcept;
[[nodiscard]] Sight sight_of(std::uint64_t band) noexcept;
// Whether a sight made for `band` still reaches as far as it should round a point at `radius`.
[[nodiscard]] bool sight_holds(std::uint64_t band, double radius) noexcept;

void add_chunk(SurfaceMesh& mesh, const World& world, Cell chunk);
void add_chunk(SurfaceMesh& mesh, const World& world, Cell chunk, const Drawn& drawn,
               std::optional<Sight> sight = std::nullopt);
// As add_chunk, but each voxel is drawn as the box it is: the faces of solid and liquid voxels
// that stand against open space, nothing smoothed or shaped from their distances.
void add_chunk_voxels(SurfaceMesh& mesh, const World& world, Cell chunk, const Drawn& drawn,
               std::optional<Sight> sight = std::nullopt);

// Whether the boxes of `chunk` and those drawn beside it tile without overlapping, as add_chunk
// needs them to; without generating anything, so a whole set of chunks is quick to check.
[[nodiscard]] bool boxes_tile(const World& world, Cell chunk, const Drawn& drawn);

} // namespace hog::globe
