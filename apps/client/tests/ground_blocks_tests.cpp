#include "sloped_ground.hpp"

#include <hog/client/ground_blocks.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

using hog::DVec3;
using hog::client::Block;
using hog::grid::Key;

namespace {

// The blocks by their nodes' codes.
using ByNode = std::unordered_map<std::uint64_t, const Block*>;

ByNode by_node(const std::vector<Block>& blocks) {
    ByNode found;
    for (const Block& block : blocks) {
        found.emplace(hog::grid::code(block.node), &block);
    }
    return found;
}

// The block holding `node`, itself or one over it.
const Block* over(const ByNode& blocks, Key node) {
    for (Key up = node;; up = up.parent()) {
        if (const auto found = blocks.find(hog::grid::code(up)); found != blocks.end()) {
            return found->second;
        }
        if (up.depth == 0) {
            return nullptr;
        }
    }
}

std::optional<Key> beside(Key node, std::size_t side) {
    hog::IVec3 step{0};
    step[side / 2] = (side % 2) == 0 ? -1 : 1;
    return node.beside(step.x, step.y, step.z);
}

// How a block's sides break the blocks' rules, tallied.
struct Balance {
    int wrong_finer = 0;   // sides marked finer or not, wrongly
    int too_coarse = 0;    // beside a block two levels finer or more
    int not_one_finer = 0; // beside finer ones not all a level finer

    void tally(const ByNode& nodes, const Block& block) {
        for (std::size_t side = 0; side < 6; ++side) {
            const auto next = beside(block.node, side);
            if (!next) {
                continue;
            }
            const Block* holding = over(nodes, *next);
            const bool finer = ((block.finer >> side) & 1U) != 0U;
            wrong_finer += finer == (holding == nullptr) ? 0 : 1;
            if (holding != nullptr) {
                too_coarse += holding->node.depth + 1 >= block.node.depth ? 0 : 1;
                continue;
            }
            // Of the node beside, its children by the side they share.
            const std::size_t facing = (side % 2) == 0 ? 1U : 0U;
            for (std::size_t octant = 0; octant < 8; ++octant) {
                if (((octant >> (side / 2)) & 1U) == facing) {
                    not_one_finer += nodes.contains(hog::grid::code(next->child(octant))) ? 0 : 1;
                }
            }
        }
    }
};

} // namespace

TEST_CASE("blocks cover the root, each beside blocks a level apart at most",
          "[client][ground_blocks]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    const DVec3 eye = grid.centre(Key{}) + DVec3{300.0, -120.0, 45.0};
    const hog::grid::View view;
    const std::uint8_t cells = 3;
    const std::vector<Block> blocks = hog::client::balanced_blocks(grid, cells, [&](Key node) {
        return view.coarse(grid, static_cast<std::uint8_t>(node.depth + cells),
                           grid.distance(node, eye));
    });
    const ByNode nodes = by_node(blocks);
    double covered = 0.0;
    Balance balance;
    for (const Block& block : blocks) {
        covered += std::ldexp(1.0, -3 * block.node.depth);
        balance.tally(nodes, block);
    }
    CHECK(balance.wrong_finer == 0);
    CHECK(balance.too_coarse == 0);
    CHECK(balance.not_one_finer == 0);
    CHECK(std::abs(covered - 1.0) < 1e-12);
    // Finest round the eye.
    const Key eye_node = grid.key_at(eye, static_cast<std::uint8_t>(grid.depth())).value_or(Key{});
    REQUIRE(eye_node.depth == grid.depth());
    const Block* at_eye = over(nodes, eye_node);
    REQUIRE(at_eye != nullptr);
    CHECK(at_eye->node.depth == grid.depth() - cells);
}

TEST_CASE("the ground is drawn in the blocks it may cross", "[client][ground_blocks]") {
    const hog::sloped::Sloped sloped;
    const DVec3 eye = hog::sloped::on_ground + (hog::sloped::normal() * 5.0);
    const std::vector<Block> blocks = hog::client::ground_blocks(sloped.pages, eye, 3);
    REQUIRE(!blocks.empty());
    for (const Block& block : blocks) {
        CAPTURE(block.node.depth, block.node.x, block.node.y, block.node.z);
        // The slope passes through it or a node beside it.
        const double size = sloped.grid.size(block.node.depth);
        CHECK(std::abs(hog::sloped::above_ground(sloped.grid.centre(block.node))) <
              size * 1.5 * std::sqrt(3.0));
    }
    // Finest under the eye.
    const Key under =
        sloped.grid.key_at(hog::sloped::on_ground, static_cast<std::uint8_t>(sloped.grid.depth()))
            .value_or(Key{});
    REQUIRE(under.depth == sloped.grid.depth());
    CHECK(std::ranges::any_of(blocks, [&](const Block& block) {
        return block.node.contains(under) && block.node.depth == sloped.grid.depth() - 3;
    }));
}
