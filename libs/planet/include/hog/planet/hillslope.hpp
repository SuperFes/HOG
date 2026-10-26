#pragma once

#include <hog/planet/level_grid.hpp>
#include <hog/planet/noise.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace hog::planet {

// How the rock at a cell weathers and stands, as fixed-point factors on the cell's weathering and
// a fixed-point slope, rise over run.
struct Weathering {
    std::int64_t rate = fixed_one;
    std::int64_t steepest = fixed_one;
};

// The rock at a fixed-point height in a cell, measured on the rock as it lay before any uplift.
using RockWeathering = std::function<Weathering(std::uint32_t cell, std::int64_t height)>;

// Per cell, for one step of hillslope change; all fixed point.
struct Hillslope {
    // Square metres of soil a slope of one carries downhill over the step, per metre across:
    // the creep diffusivity times the step's length, set by climate.
    std::span<const std::int64_t> creep;
    // Metres of bare bedrock that weather into soil over the step, before the rock's factor.
    std::span<const std::int64_t> weathering;
    // How the rock under the soil weathers and stands; all alike if unset.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    RockWeathering rock = {};
    // Metres each cell has risen so far, so the rock is read where it now lies; none if empty.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<const std::int64_t> risen = {};
};

// One step of hillslope change over the land, changing `heights` and the `soil` on them, both
// fixed-point metres per cell. Bedrock weathers into soil quickest where it's bare, slowing as
// the soil thickens. Ground steeper than its rock holds falls to the ground below, as cliffs
// collapse into scree, the fallen rock lying loose. Soil creeps downhill, ever faster as slopes
// near the steepest the rock holds (after Roering, Kirchner and Dietrich 1999), but only as
// much as there is: bare rock doesn't creep, so where weathering can't keep up the ground stays
// sharp and rocky, and where soil lies thick it rounds over. Ground at or below the sea's level
// gives none up. The arithmetic is integer, or double using only correctly rounded operations
// in a fixed order, so the result is the same everywhere.
void wear_slopes(const LevelGrid& grid, std::vector<std::int64_t>& heights,
                 std::vector<std::int64_t>& soil, const Hillslope& hillslope,
                 std::int64_t sea_level = 0);

} // namespace hog::planet
