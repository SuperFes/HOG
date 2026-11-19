#pragma once

#include <hog/core/result.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/tree.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::grid {

// The trees under one node as they travel: the node, the generator version that made them, and
// each kind's tree from the node down, as deep as it goes; a piece may lack any of them. Put in at
// its node, it stands in place of what was held there.
struct Piece {
    static constexpr std::uint8_t format = 1;

    Key root;
    std::uint32_t generator = 0;
    std::optional<Tree<Shape>> shape;
    std::optional<Tree<Material>> material;
    std::optional<Tree<Water>> water;
    std::optional<Tree<Woods>> woods;

    // Little-endian: format, the root's depth and x, y, z, the generator version, a byte saying
    // which trees follow (1 shape, 2 material, 4 water, 8 woods), then each in that order: a
    // varint length and its bytes.
    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't a piece, or a tree going deeper than the grid can.
    [[nodiscard]] static Result<Piece> decode(std::span<const std::byte> bytes);
};

} // namespace hog::grid
