#pragma once

#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>

#include <cstdint>
#include <vector>

namespace hog::grid {

// How finely the grid is seen, as the client draws it: a node may span so many pixels of a
// picture 900 high, through a vertical field of view of a radian, before finer is wanted. The
// server sends by it and the client draws by it, so they agree. Materials blend across triangles,
// so nodes 16 pixels across look much as 8 did, at a quarter of the nodes.
struct View {
    static constexpr double node_pixels = 16.0;
    static constexpr double picture_height = 900.0;
    static constexpr double fov = 1.0;

    explicit View(double pixels = node_pixels) noexcept : angle{pixels * fov / picture_height} {}

    double angle; // radians a node may span

    // Whether nodes `depth` deep seen from `metres` away span more than allowed.
    [[nodiscard]] bool coarse(const Grid& grid, std::uint8_t depth, double metres) const noexcept;
    // Levels of the page rooted at `root` seen from `metres` away: down to nodes spanning no more
    // than allowed, but at least one.
    [[nodiscard]] std::uint8_t levels_at(const Grid& grid, Key root, double metres) const noexcept;
    // Into `pages`, the pages at `foot`, the foot of a page, under an open leaf of it at `leaf`
    // holding `surface`, worth seeing from `eye`: wherever nodes at the foot look coarse from
    // there. A leaf above the foot the surface misses is open only in case the ground lies nearer
    // than its distance says, and one in the solid only for caves, so either is followed only
    // where the finest nodes are wanted; one the surface crosses is followed as any at the foot.
    void under(const Grid& grid, Key leaf, std::uint8_t foot, const Surface& surface, DVec3 eye,
               std::vector<Key>& pages) const;
};

} // namespace hog::grid
