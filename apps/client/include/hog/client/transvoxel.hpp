#pragma once

#include <hog/client/shape_mesh.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>

#include <array>
#include <cstdint>
#include <functional>

namespace hog::client {

// Where the ground crosses an edge: metres from the planet's centre, the way it faces out of the
// solid, and what it shows, as ShapeVertex has them.
struct Crossed {
    DVec3 at{0.0};
    Vec3 normal{0.0F};
    std::array<std::uint16_t, 2> materials{};
    float share = 1.0F;
};

// The ground as a block reads it, at points of the grid's finest lattice: the corners of the
// finest nodes, the root's low corner at nothing. Each answer may depend on the point alone, so
// blocks side by side, or drawn at different depths, meet where they share points.
struct BlockField {
    // Below nothing where solid.
    std::function<double(I64Vec3)> value;
    // Where the ground crosses the edge from `low` to `high`, along one axis, whose values
    // differ in sign.
    std::function<Crossed(I64Vec3 low, I64Vec3 high)> crossing;
};

// A node drawn as cells `cells` levels under it, 1 << cells a side, and the sides where the block
// beside it is drawn a level finer: bit 2 * axis for the low side along that axis, the next for
// the high side.
struct Block {
    grid::Key node;
    std::uint8_t cells = 0;
    std::uint8_t finer = 0;
};

// The ground through a block, by Lengyel's Transvoxel: marching cubes over its cells, with
// transition cells along its finer sides to meet the finer blocks there. Triangles anticlockwise
// seen from the open side; the mesh's origin is the node's low corner. Precondition: the cells no
// deeper than the grid's finest nodes, and a level shallower where any side is finer.
[[nodiscard]] ShapeMesh mesh_block(const grid::Grid& grid, const Block& block,
                                   const BlockField& field);

} // namespace hog::client
