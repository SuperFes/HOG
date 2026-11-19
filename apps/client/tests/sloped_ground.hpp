#pragma once

#include <hog/client/held_pieces.hpp>
#include <hog/core/math.hpp>
#include <hog/globe/material.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/piece.hpp>
#include <hog/grid/tree.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

// A sloping ground held finely under one node and coarsely all round it, for tests.
namespace hog::sloped {

using grid::Key;

// 1 m nodes 11 deep: a node of 64 m whose low corner lies 64 m below the planet's centre on x.
inline constexpr Key piece_root{.depth = 5, .x = 15, .y = 16, .z = 16};
inline constexpr I64Vec3 piece_low{15 * 64, 16 * 64, 16 * 64};
inline constexpr DVec3 slope{0.2, 0.3, 1.0};
inline constexpr DVec3 on_ground{-30.0, 32.0, 26.3};

inline DVec3 normal() {
    return glm::normalize(slope);
}

inline double above_ground(DVec3 point) {
    return glm::dot(point - on_ground, normal());
}

// A slope under `root`, `levels` deep, as its finest nodes have it.
inline std::shared_ptr<const hog::grid::Piece> sloped(const hog::grid::Grid& grid, Key root,
                                                      std::uint8_t levels) {
    const DVec3 low = grid.low(root);
    const double side = grid.size(static_cast<std::uint8_t>(root.depth + levels));
    auto piece = std::make_shared<hog::grid::Piece>();
    piece->root = root;
    piece->generator = 1;
    piece->shape = hog::grid::Tree<hog::grid::Shape>::build(levels, [&](int x, int y, int z) {
        const DVec3 middle = low + (DVec3{x + 0.5, y + 0.5, z + 0.5} * side);
        return hog::grid::Surface::plane(normal(), above_ground(middle) / side,
                                         static_cast<std::uint16_t>(hog::globe::Material::Granite));
    });
    return piece;
}

// The root's piece, 5 levels deep.
inline std::shared_ptr<const hog::grid::Piece> sloped_root(const hog::grid::Grid& grid) {
    return sloped(grid, Key{}, 5);
}
// The finer piece, to the finest.
inline std::shared_ptr<const hog::grid::Piece> sloped_finely(const hog::grid::Grid& grid) {
    return sloped(grid, piece_root, 6);
}

// The slope held finely under one node, and coarsely all round it.
struct Sloped {
    hog::grid::Grid grid{1000.0, 1.0};
    client::PiecesSnapshot pieces{grid,
                                  {{Key{}, sloped_root(grid)}, {piece_root, sloped_finely(grid)}}};
};

inline DVec3 metres(const hog::grid::Grid& grid, I64Vec3 point) {
    return grid.low(Key{}) + DVec3{point};
}

} // namespace hog::sloped
