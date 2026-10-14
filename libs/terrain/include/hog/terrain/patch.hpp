#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/world.hpp>

#include <cstdint>
#include <vector>

namespace hog::terrain {

// A patch's vertices lie on the corners of its cells this many levels down.
inline constexpr int patch_depth = 5;
inline constexpr int patch_cells = 1 << patch_depth; // across a side
inline constexpr int patch_side = patch_cells + 1;   // vertices across a side
inline constexpr int max_patch_level = planet::max_level - patch_depth;

// A corner of the cells at `level` on a face, with i and j from 0 to 2^level inclusive.
struct Corner {
    int face = 0;
    std::int64_t i = 0;
    std::int64_t j = 0;
    int level = 0;

    friend constexpr bool operator==(const Corner&, const Corner&) = default;
};

// The corner as named on the lowest of the faces it lies on, so every face sharing it names it
// alike. Either of i and j may lie one step off the face, which names the point that step onto
// the neighbouring face, folded over the edge.
[[nodiscard]] Corner canonical(Corner corner);

// Unit direction from the planet's centre. Corners naming one point give the same bits, however
// they name it.
[[nodiscard]] DVec3 direction(Corner corner);

// The leaf cell a corner takes its ground from: on its lowest face, the leaf whose lowest corner
// it is, or the last one at the face's far edges. The same at every level the corner lies on.
[[nodiscard]] planet::CellId sample_cell(Corner corner);

struct PatchVertex {
    Vec3 position; // metres from the patch's origin
    Vec3 normal;
    planet::Biome biome = planet::Biome::Ocean;
};

// Metres from sea level.
struct HeightRange {
    double lowest = 0.0;
    double highest = 0.0;

    friend constexpr bool operator==(const HeightRange&, const HeightRange&) = default;
};

struct Patch {
    planet::CellId cell;
    WorldPosition origin; // over the patch's centre at sea level
    HeightRange heights;  // of its vertices
    // patch_side² vertices row by row, i fastest, from the corner at the lowest i and j.
    std::vector<PatchVertex> vertices;
};

// The ground of a cell at most max_patch_level deep, changes and digging included.
[[nodiscard]] Patch bake_patch(const planet::World& world, planet::CellId cell);

// Whether a change to a cell can alter a patch's bake: whether it overlaps the patch or a cell
// beside it, which the border and far edges sample. A change anywhere in a cell beside it counts,
// being cheaper to bake again than to rule out.
[[nodiscard]] bool reaches(planet::CellId patch, planet::CellId change);

} // namespace hog::terrain
