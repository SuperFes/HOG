#pragma once

#include <hog/client/held_pieces.hpp>
#include <hog/client/transvoxel.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/view.hpp>
#include <hog/grid/water_bodies.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace hog::client {

// Blocks over all of the grid's root, `cells` levels of cells each: from the root down, each
// node split where `split` says, then where a block beside it is more than a level finer, so
// blocks beside each other differ by a level at most; each with its finer sides. In order of
// their nodes. `split` is asked only of nodes whose cells lie a level or more over the finest.
[[nodiscard]] std::vector<Block> balanced_blocks(const grid::Grid& grid, std::uint8_t cells,
                                                 const std::function<bool(grid::Key)>& split);

// The blocks the held ground is drawn in, seen from `eye`: split where their cells look coarse
// from there as `view` has it, but not where the ground's the same all through; and of those,
// the ones the ground or the top of the water may cross.
[[nodiscard]] std::vector<Block> ground_blocks(const PiecesSnapshot& pieces,
                                               const grid::WaterBodies& bodies, DVec3 eye,
                                               std::uint8_t cells,
                                               const grid::View& view = grid::View{});

} // namespace hog::client
