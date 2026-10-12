#pragma once

#include <hog/planet/level_grid.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// Where water runs over a height field. Every cell drains to one neighbour, and the chain ends at
// an outlet: the sea, or the lowest cell of a world without one. Ground that holds water fills to
// its lowest rim and drains across it.
struct Drainage {
    // Fixed-point height of the water where there is some (the sea level over the sea, a lake's
    // level over a lake) and of the ground elsewhere. A lake's level climbs by a hair per cell
    // towards its outlet, so every inland cell has a lower neighbour.
    std::vector<std::int64_t> surface;
    // The neighbour each cell drains to, down the steepest slope of the surface; an outlet's is
    // itself.
    std::vector<std::uint32_t> receiver;
    // Outlets first, then every cell after the one it drains to.
    std::vector<std::uint32_t> order;
};

// The sea covers every cell at or below `sea_level`, all fixed point.
[[nodiscard]] Drainage drain(const LevelGrid& grid, std::span<const std::int64_t> heights,
                             std::int64_t sea_level = 0);

// What collects at each cell: its own share plus everything upstream of it.
[[nodiscard]] std::vector<std::int64_t> accumulate(const Drainage& drainage,
                                                   std::span<const std::int64_t> local);

} // namespace hog::planet
