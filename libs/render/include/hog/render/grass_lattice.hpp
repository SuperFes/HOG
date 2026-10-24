#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>

#include <cstdint>

namespace hog::render {

// The fixed lattice grass grows on, a blade at most in each of its cells: as fine as the thickest
// grass grows, so each blade has its own spot in the world, whichever patch of ground draws it.
inline constexpr double finest_grass_cell = 0.09; // metres across, at most
[[nodiscard]] int grass_lattice_level(double planet_radius);

// A world's grass lattice: its level, and the seed its dice are cast from.
struct GrassLattice {
    int level = 0;
    std::uint32_t seed = 0;
};

[[nodiscard]] GrassLattice grass_lattice(double planet_radius, std::uint64_t world_seed);

// A cell's own dice, at any level, from the world's `seed`.
[[nodiscard]] std::uint32_t grass_hash(planet::FaceIJ cell, std::uint32_t seed) noexcept;

// The lattice cell, at `lattice_level`, whose blade stands for all of `cell`'s when grass is
// thinned to a blade in each cell of its level: its picked child's, picked down to the lattice,
// so the blades thinner grass keeps are always some of those thicker grass grows.
[[nodiscard]] planet::FaceIJ standing_for(planet::FaceIJ cell, int lattice_level,
                                          std::uint32_t seed) noexcept;

// How many levels up from it the blade in lattice cell `cell` stands for its cells, `most` at
// most: grass thinned to a blade in each cell that many levels up keeps it.
[[nodiscard]] int stands_up(planet::FaceIJ cell, std::uint32_t seed, int most) noexcept;

// The `which`th, 0 to 2, of `cell`'s children whose blades it doesn't stand for: each child's blade
// stands for no cell further up than it.
[[nodiscard]] planet::FaceIJ unpicked_child(planet::FaceIJ cell, int which,
                                            std::uint32_t seed) noexcept;

// The dice the blade in lattice cell `cell` is thinned by, looking `most` levels up at most: cast
// at the cell just over the highest it stands for, so the same whichever of those it's sown from,
// and found on the way up to it.
[[nodiscard]] std::uint32_t thinning_dice(planet::FaceIJ cell, std::uint32_t seed,
                                          int most) noexcept;

// Where in its lattice cell the blade of `hash`, as grass_hash has it, stands: s and t, 0 to 1,
// as the face's.
[[nodiscard]] Vec2 grass_spot(std::uint32_t hash) noexcept;

// The clumps sparse grass grows in: corners this many levels up the lattice from its blades, each
// on or off, the grass growing where they blend to over half, so lone corners grow round tufts
// and neighbours join into bands and fields; its edges roughened by finer corners.
inline constexpr int clump_levels = 5;
inline constexpr int clump_wobble_levels = 3;

// How far into a clump the blade of lattice cell `blade`, at `spot` in it, stands: above 0 within
// one, below 0 outside, where each clump corner is on with chance `on`.
[[nodiscard]] float clumped(planet::FaceIJ blade, Vec2 spot, std::uint32_t seed, float on) noexcept;

// The chance each clump corner is on for clumps to cover `share` of the ground.
[[nodiscard]] float clump_corners_on(float share) noexcept;

} // namespace hog::render
