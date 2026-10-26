#pragma once

#include <hog/planet/drainage.hpp>
#include <hog/planet/level_grid.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// Per cell, for one step of landscape evolution; all fixed point.
struct Erosion {
    // Metres the ground rises.
    std::span<const std::int64_t> uplift;
    // How readily rivers cut the ground in one step: the stream-power K times the step's length,
    // per square root of square metres drained.
    std::span<const std::int64_t> erodibility;
    // The share of rain falling on the cell that runs off into rivers, from 0 to 1.
    std::span<const std::int64_t> runoff;
    // Keys which way water wanders down the slopes, as drain takes it; 0 always takes the
    // steepest.
    std::uint64_t wander = 0;
};

// One step of uplift and river erosion (stream power with n = 1 and m = 1/2, solved implicitly
// downstream to upstream, after Braun and Willett 2013); returns the drainage it eroded along.
// Ground at or below the sea's level never rises, the sea's cells stay as they are and land never
// cuts below the water or the ground it drains into, so valleys run down to a lake's level rather
// than its bed. Ground under a lake isn't cut, but its outlet is, so lakes drain as their rims
// wear down. The arithmetic is integer, or
// double using only correctly rounded operations, so the result is the same everywhere.
Drainage erode(const LevelGrid& grid, std::vector<std::int64_t>& heights, const Erosion& erosion,
               std::int64_t sea_level = 0);

} // namespace hog::planet
