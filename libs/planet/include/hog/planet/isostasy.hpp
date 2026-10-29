#pragma once

#include <hog/planet/level_grid.hpp>
#include <hog/planet/tectonics.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// Ocean floor sinks this many metres below a ridge's crest as it cools, by the square root of its
// age, until it's this many steps old.
inline constexpr double cooled_depth = 3'000.0;
inline constexpr double cooling_steps = 60.0;
// Metres continental crust as thick as it settles to stands above a ridge's crest.
inline constexpr double continent_freeboard = 3'400.0;
// Metres a column rises for each metre it thickens, by how much lighter its crust is than the
// mantle it floats on.
inline constexpr double continental_buoyancy = 0.15;
inline constexpr double oceanic_buoyancy = 0.12;
// Of the planet, the share of continental crust the sea floods round the land's edges.
inline constexpr double shelf_share = 0.1;
// Metres deep a world's water would stand spread evenly over it, as it has to begin with.
inline constexpr double water_layer = 2'600.0;
// Steps the plates move before the crust is taken as settled.
inline constexpr int settling_steps = 60;

// Fixed-point metres each grid cell's crust stands above the crest of a mid-ocean ridge.
[[nodiscard]] std::vector<std::int64_t> crust_heights(const Tectonics& tectonics);

// Cubic metres of water the ground holds below `level`, all heights fixed point.
[[nodiscard]] double water_below(const LevelGrid& grid, std::span<const std::int64_t> heights,
                                 std::int64_t level);
// The level `water` cubic metres fill the lowest ground to.
[[nodiscard]] std::int64_t sea_level(const LevelGrid& grid, std::span<const std::int64_t> heights,
                                     double water);
// Cubic metres of water a world has to begin with.
[[nodiscard]] double starting_water(const LevelGrid& grid);
// `water`, or if that leaves less than `ocean_fraction` of the settled ground under the sea, as
// much more as it takes, come from space.
[[nodiscard]] double topped_up(const LevelGrid& grid, std::span<const std::int64_t> heights,
                               double water, double ocean_fraction);
// The level with `fraction` of the planet's area below it.
// Precondition: 0 <= fraction <= 1.
[[nodiscard]] std::int64_t level_covering(const LevelGrid& grid,
                                          std::span<const std::int64_t> heights, double fraction);

// The crust as the plates leave it, floated, and the level its water fills the sea to.
struct Settled {
    Tectonics tectonics;
    // Fixed-point metres above the crest of a mid-ocean ridge, to each of its grid's cells.
    std::vector<std::int64_t> heights;
    std::int64_t sea_level = 0;
};
// Moves the plates `steps` times from where `settings` starts them, starting with a natural
// amount of water and topping it up to cover `ocean_fraction` of the planet if it falls short.
[[nodiscard]] Settled settle(std::uint64_t seed, double radius, Tectonics::Settings settings,
                             double ocean_fraction, int steps = settling_steps);

// Of the crust, the share that starts continental for `ocean_fraction` of the planet to start
// under the sea: the land, and the shelves round it.
[[nodiscard]] double continental_share(double ocean_fraction);

} // namespace hog::planet
