#pragma once

#include <hog/client/held_pieces.hpp>
#include <hog/core/math.hpp>
#include <hog/flora/placement.hpp>
#include <hog/globe/material.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/piece.hpp>
#include <hog/grid/tree.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>

// Wooded ground for tests: a plane through a piece held finely, with woods on it.
namespace hog::planted {

using client::PiecesSnapshot;
using grid::Key;

// 1 m nodes 12 deep; a piece 64 m across, 1 km out along z, its ground a plane through `on`.
inline constexpr double reach = 2048.0;
inline constexpr Key piece_root{.depth = 6, .x = 32, .y = 32, .z = 47};
inline constexpr DVec3 on{32.0, 32.0, 990.0};

struct Ground {
    DVec3 normal;
    PiecesSnapshot pieces;

    [[nodiscard]] double above(DVec3 point) const { return glm::dot(point - on, normal); }
};

// The piece, its ground facing `slope`, its woods all of the one kind at `cover`.
inline std::shared_ptr<const grid::Piece> piece_of(DVec3 slope, double cover) {
    const DVec3 normal = glm::normalize(slope);
    const auto above = [&](DVec3 point) { return glm::dot(point - on, normal); };
    const grid::Grid grid{reach, 1.0};
    const DVec3 low = grid.low(piece_root);
    auto piece = std::make_shared<grid::Piece>();
    piece->root = piece_root;
    piece->generator = 1;
    piece->shape = grid::Tree<grid::Shape>::build(6, [&](int x, int y, int z) {
        const DVec3 middle = low + DVec3{x + 0.5, y + 0.5, z + 0.5};
        return grid::Surface::plane(normal, above(middle),
                                    static_cast<std::uint16_t>(globe::Material::Granite));
    });
    piece->woods = grid::Tree<grid::Woods>::build(2, [&](int x, int y, int z) {
        const DVec3 middle = low + (DVec3{x + 0.5, y + 0.5, z + 0.5} * 16.0);
        if (std::abs(above(middle)) > 8.0 * std::numbers::sqrt3) {
            return grid::Stand{};
        }
        return grid::Stand::of(cover, {0.0, 1.0, 0.0, 0.0, 0.0, 0.0});
    });
    return piece;
}

// The same ground coarsely over all of the root, as a client always holds it.
inline std::shared_ptr<const grid::Piece> root_of(DVec3 slope) {
    const DVec3 normal = glm::normalize(slope);
    const grid::Grid grid{reach, 1.0};
    const DVec3 low = grid.low(Key{});
    const double side = grid.size(5);
    auto piece = std::make_shared<grid::Piece>();
    piece->generator = 1;
    piece->shape = grid::Tree<grid::Shape>::build(5, [&](int x, int y, int z) {
        const DVec3 middle = low + (DVec3{x + 0.5, y + 0.5, z + 0.5} * side);
        return grid::Surface::plane(normal, glm::dot(middle - on, normal) / side,
                                    static_cast<std::uint16_t>(globe::Material::Granite));
    });
    return piece;
}

inline Ground ground_of(DVec3 slope, double cover) {
    return {.normal = glm::normalize(slope),
            .pieces =
                PiecesSnapshot{grid::Grid{reach, 1.0},
                               {{Key{}, root_of(slope)}, {piece_root, piece_of(slope, cover)}}}};
}

} // namespace hog::planted
