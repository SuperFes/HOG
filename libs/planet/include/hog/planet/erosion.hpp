#pragma once

#include <hog/planet/drainage.hpp>
#include <hog/planet/level_grid.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace hog::planet {

// A fixed-point factor on a cell's erodibility for the rock at a fixed-point height in it,
// measured on the rock as it lay before any uplift.
using Yielding = std::function<std::int64_t(std::uint32_t cell, std::int64_t height)>;

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
    // How the rock rivers reach yields to them; all alike if unset.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    Yielding yielding = {};
    // Metres each cell has risen so far, which the step adds to, so the rock is read where it
    // now lies; none if empty.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<std::int64_t> risen = {};
    // Whether ground under the sea rises too, so it can lift out of it.
    bool seabed_rises = false;
    // Whether the rock rivers wear off is laid in the lakes they run into, filling them from where
    // they flow in up to their levels, so basins fill into plains; otherwise it's lost.
    bool fills_basins = false;
    // Fixed-point metres the sea stands at in each step of a landform's evolution, as ice ages
    // draw it down; at its level throughout if empty.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<const std::int64_t> sea_levels = {};
    // Metres a step waves cut back a coast of middling rock open to them; none if zero.
    double wave_retreat = 0.0;
    // The share of each metre worn off the ground that it floats back up by, as the lightened
    // crust rises on the mantle; none if zero.
    double rebound = 0.0;
    // A grid as coarse as the plates bend over, which the rebound is spread across, so a range
    // worn down rises back but a valley cut in it doesn't alone; needed if there is a rebound.
    const LevelGrid* bending = nullptr;
};

// One step of uplift and river erosion (stream power with n = 1 and m = 1/2, solved implicitly
// downstream to upstream, after Braun and Willett 2013); returns the drainage it eroded along.
// Ground at or below the sea's level rises only if `erosion` lets it, the sea's cells are never
// worn and land never cuts below the water or the ground it drains into, so valleys run down to a
// lake's level rather than its bed. Ground under a lake isn't cut, but its outlet is, so lakes
// drain as their rims wear down. The arithmetic is integer, or double using only correctly rounded
// operations, so the result is the same everywhere.
Drainage erode(const LevelGrid& grid, std::vector<std::int64_t>& heights, const Erosion& erosion,
               std::int64_t sea_level = 0);

// One step of waves wearing the coast: each cell standing above the sea beside it is cut back,
// as its cliff retreats across it, towards a platform a little under the sea's level. Cliffs
// open to more of the sea, over deeper water, and of softer rock as `erosion` yields it retreat
// faster; waves crossing a platform they cut arrive spent. Returns the fixed-point metres cut
// from each cell.
std::vector<std::int64_t> wear_coast(const LevelGrid& grid, std::vector<std::int64_t>& heights,
                                     const Erosion& erosion, std::int64_t sea_level = 0);

// Fixed-point metres each cell of `grid` floats back up by as the crust under it is lightened by
// `unloaded`, fixed-point metres worn off each cell, or weighed down where it's negative, laid
// on: `erosion`'s rebound share of the load, spread over the cells of its bending grid.
std::vector<std::int64_t> rebound(const LevelGrid& grid, std::span<const std::int64_t> unloaded,
                                  const Erosion& erosion);

} // namespace hog::planet
