#pragma once

#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/tree.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace hog::client {

// A corner of the ground's surface: metres from the mesh's origin, the way it faces out of the
// solid, and the two materials it shows, as palette entries, with how much of it is the first.
struct ShapeVertex {
    Vec3 position{0.0F};
    Vec3 normal{0.0F};
    std::array<std::uint16_t, 2> materials{};
    float share = 1.0F;
};

// Triangles anticlockwise seen from the open side.
struct ShapeMesh {
    DVec3 origin{0.0}; // metres from the planet's centre: the page's low corner
    std::vector<ShapeVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// The smooth surface through the shape tree `tree` of the page rooted at `root`, drawn `levels`
// deep, by dual contouring: a vertex in each leaf the surface passes, on its plane where it has
// one, joined across every edge inside the page where solid meets open. Leaves are drawn no wider
// than four of the drawn depth's nodes. Whether a corner is solid is read from the leaves of the
// page touching it, so the mesh depends on the page and depth alone; the edges on the page's sides
// are left to what draws its neighbours. Precondition: levels <= tree.levels().
[[nodiscard]] ShapeMesh mesh_shape(const grid::Grid& grid, grid::Key root,
                                   const grid::Tree<grid::Shape>& tree, std::uint8_t levels);

} // namespace hog::client
