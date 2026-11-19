#include <hog/client/ground_blocks.hpp>
#include <hog/core/assert.hpp>
#include <hog/grid/kinds.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <unordered_set>

namespace hog::client {
namespace {

// The blocks as their nodes' codes.
using Leaves = std::unordered_set<std::uint64_t>;

// Whether blocks lie under the node, finer than it.
bool is_split(const Leaves& leaves, grid::Key node) {
    for (grid::Key up = node;; up = up.parent()) {
        if (leaves.contains(grid::code(up))) {
            return false;
        }
        if (up.depth == 0) {
            return true;
        }
    }
}

// The node beside `node` on a side: bit 0 of `side` for the high side, the rest its axis.
std::optional<grid::Key> beside(grid::Key node, std::size_t side) {
    IVec3 step{0};
    step[side / 2] = (side % 2) == 0 ? -1 : 1;
    return node.beside(step.x, step.y, step.z);
}

// The block holding `node`, itself or one over it, if any.
std::optional<grid::Key> block_over(const Leaves& leaves, grid::Key node) {
    for (grid::Key up = node;; up = up.parent()) {
        if (leaves.contains(grid::code(up))) {
            return up;
        }
        if (up.depth == 0) {
            return std::nullopt;
        }
    }
}

// Whether a block must split for the blocks beside it on a side to lie a level finer at most.
bool too_coarse(const Leaves& leaves, grid::Key node, std::size_t side) {
    const auto next = beside(node, side);
    if (!next || !is_split(leaves, *next)) {
        return false;
    }
    const std::size_t axis = side / 2;
    // Of the node beside, its children by the side they share.
    const std::size_t facing = (side % 2) == 0 ? 1U : 0U;
    for (std::size_t octant = 0; octant < 8; ++octant) {
        if (((octant >> axis) & 1U) == facing && is_split(leaves, next->child(octant))) {
            return true;
        }
    }
    return false;
}

// The nodes from the root down, split where `split` says.
Leaves refined(std::uint8_t deepest, const std::function<bool(grid::Key)>& split) {
    Leaves leaves;
    std::vector<grid::Key> pending{grid::Key{}};
    while (!pending.empty()) {
        const grid::Key node = pending.back();
        pending.pop_back();
        if (node.depth < deepest && split(node)) {
            for (std::size_t octant = 0; octant < 8; ++octant) {
                pending.push_back(node.child(octant));
            }
        } else {
            leaves.insert(grid::code(node));
        }
    }
    return leaves;
}

// Splits blocks till those beside each other differ by a level at most.
void balance(Leaves& leaves) {
    std::vector<grid::Key> pending;
    pending.reserve(leaves.size());
    for (const std::uint64_t code : leaves) {
        pending.push_back(grid::from_code(code));
    }
    while (!pending.empty()) {
        const grid::Key node = pending.back();
        pending.pop_back();
        if (!leaves.contains(grid::code(node))) {
            continue;
        }
        bool coarse = false;
        for (std::size_t side = 0; side < 6 && !coarse; ++side) {
            coarse = too_coarse(leaves, node, side);
        }
        if (!coarse) {
            continue;
        }
        leaves.erase(grid::code(node));
        for (std::size_t octant = 0; octant < 8; ++octant) {
            leaves.insert(grid::code(node.child(octant)));
            pending.push_back(node.child(octant));
        }
        // Those beside it may now lie two levels coarser than its children.
        for (std::size_t side = 0; side < 6; ++side) {
            const auto next = beside(node, side);
            if (const auto over = next ? block_over(leaves, *next) : std::nullopt) {
                pending.push_back(*over);
            }
        }
    }
}

// What's held over a node, where it's the same all through: the leaf over it holds no plane, or
// one that misses it.
std::optional<grid::Surface> uniform(const PiecesSnapshot& pieces, grid::Key node) {
    const auto known = pieces.at<grid::Shape>(node);
    if (!known) {
        return grid::Surface::open();
    }
    const grid::Surface& value = known->found.value;
    if (!value.crosses()) {
        return value;
    }
    if (known->found.depth >= node.depth) {
        return std::nullopt;
    }
    const grid::Grid& grid = pieces.grid();
    const grid::Key leaf = node.ancestor(known->found.depth);
    const DVec3 normal = value.normal();
    const double away = (value.distance() * grid.size(leaf.depth)) +
                        glm::dot(normal, grid.centre(node) - grid.centre(leaf));
    const double reach = 0.5 * grid.size(node.depth) *
                         (std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z));
    if (std::abs(away) < reach) {
        return std::nullopt;
    }
    return away < 0.0 ? grid::Surface::solid() : grid::Surface::open();
}

// Whether the ground may cross a block: where it or a node beside it isn't one thing all
// through, or they aren't all the same thing.
bool may_cross(const PiecesSnapshot& pieces, grid::Key node) {
    const auto whole = uniform(pieces, node);
    if (!whole) {
        return true;
    }
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto next = node.beside(dx, dy, dz);
                const auto near = next ? uniform(pieces, *next) : whole;
                if (!near || *near != *whole) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

std::vector<Block> balanced_blocks(const grid::Grid& grid, std::uint8_t cells,
                                   const std::function<bool(grid::Key)>& split) {
    HOG_ASSERT(cells <= grid.depth(), "a block's cells within the grid");
    Leaves leaves = refined(static_cast<std::uint8_t>(grid.depth() - cells), split);
    balance(leaves);
    std::vector<Block> blocks;
    blocks.reserve(leaves.size());
    for (const std::uint64_t code : leaves) {
        Block block{.node = grid::from_code(code), .cells = cells, .finer = 0};
        for (std::size_t side = 0; side < 6; ++side) {
            if (const auto next = beside(block.node, side); next && is_split(leaves, *next)) {
                block.finer = static_cast<std::uint8_t>(block.finer | (1U << side));
            }
        }
        blocks.push_back(block);
    }
    std::ranges::sort(blocks, {}, &Block::node);
    return blocks;
}

std::vector<Block> ground_blocks(const PiecesSnapshot& pieces, DVec3 eye, std::uint8_t cells,
                                 const grid::View& view) {
    const grid::Grid& grid = pieces.grid();
    std::vector<Block> blocks = balanced_blocks(grid, cells, [&](grid::Key node) {
        if (const auto known = pieces.at<grid::Shape>(node); known && !known->found.open &&
                                                             known->found.depth <= node.depth &&
                                                             uniform(pieces, node)) {
            return false;
        }
        return view.coarse(grid, static_cast<std::uint8_t>(node.depth + cells),
                           grid.distance(node, eye));
    });
    std::erase_if(blocks, [&](const Block& block) { return !may_cross(pieces, block.node); });
    return blocks;
}

} // namespace hog::client
