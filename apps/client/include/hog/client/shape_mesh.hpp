#pragma once

#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/tree.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hog::client {

// A corner of the ground's surface: metres from the mesh's origin, the way it faces out of the
// solid, and the two materials it shows, as palette entries, with how much of it is the first.
// A normal of nothing is one not yet known.
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
    // A page's own: the vertex of each leaf the surface meets along its sides and round the
    // pages drawn in it, by the leaf's key's code, for its seam and its neighbours' to meet;
    // not all are in its triangles.
    std::unordered_map<std::uint64_t, ShapeVertex> border;
};

// A held page as drawn: its shape tree, how deep, and the pages drawn in place of its finest
// nodes, in order; with its mesh, once meshed, for seams to meet.
struct ShapeDrawn {
    grid::Key page;
    const grid::Tree<grid::Shape>* tree = nullptr;
    std::uint8_t levels = 0;
    std::span<const grid::Key> under;
    const ShapeMesh* mesh = nullptr;
};

// The smooth surface through a page's shape tree drawn `levels` deep, by dual contouring: a
// vertex in each leaf the surface passes, on its plane where it has one, joined across the edges
// where solid meets open. Leaves are drawn no wider than four of the drawn depth's nodes, and
// split round the pages drawn under it. Edges touching the page's sides or those pages are left
// to the seams, so the mesh depends on the page, its depth and what's drawn under it alone.
// Precondition: levels <= tree.levels(); pages under it only where drawn as deep as it goes.
[[nodiscard]] ShapeMesh mesh_shape(const grid::Grid& grid, const ShapeDrawn& page);
[[nodiscard]] ShapeMesh mesh_shape(const grid::Grid& grid, grid::Key root,
                                   const grid::Tree<grid::Shape>& tree, std::uint8_t levels);

// The pages drawn, by their roots, each place drawn by one, for meshing the seams between them.
class ShapesDrawn {
public:
    explicit ShapesDrawn(const grid::Grid& grid) : grid_{grid} {}

    // Precondition: its tree outlives this, and its mesh, if any.
    void add(const ShapeDrawn& page);

    [[nodiscard]] const grid::Grid& grid() const noexcept { return grid_; }
    [[nodiscard]] const ShapeDrawn* page(grid::Key root) const;
    // The page drawing `node`: the deepest whose root is it or holds it; none where none is.
    [[nodiscard]] const ShapeDrawn* over(grid::Key node) const;
    // The pages whose places touch the place `root` draws, at a side, an edge or a corner, or
    // round the pages drawn in it: what its seam reads. Precondition: `root` drawn.
    [[nodiscard]] std::vector<grid::Key> touching(grid::Key root) const;

private:
    struct Hash {
        [[nodiscard]] std::size_t operator()(grid::Key key) const noexcept {
            return static_cast<std::size_t>(grid::code(key));
        }
    };

    // Into `found`, the pages drawn in `page`, and in those, that `wanted` says touch.
    void nested(const ShapeDrawn& page, const std::function<bool(grid::Key)>& wanted,
                std::unordered_set<grid::Key, Hash>& seen, std::vector<grid::Key>& found) const;

    grid::Grid grid_;
    std::unordered_map<grid::Key, ShapeDrawn, Hash> pages_;
};

// The page's seam: polygons over the edges touching its sides and the pages drawn in it, joining
// its leaves to those beside them as both are drawn, with the vertices of the meshes either side,
// so they close the gaps between them. Each edge on a side between places is drawn by the seam
// of the place holding the lowest of the four leaves round it. Precondition: `root` drawn; where
// a page isn't yet meshed, its leaves' vertices are placed as the seam finds them.
[[nodiscard]] ShapeMesh mesh_seam(const ShapesDrawn& drawn, grid::Key root);

} // namespace hog::client
