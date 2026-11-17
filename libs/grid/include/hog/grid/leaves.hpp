#pragma once

#include <hog/grid/key.hpp>
#include <hog/grid/tree.hpp>

#include <cstdint>
#include <vector>

namespace hog::grid {

// Calls visit(key, found) for each open leaf of `tree`, a page rooted at `root`, with its key in
// the grid and what the tree says of it.
template <typename Kind, typename Visit>
void for_open_leaves(const Tree<Kind>& tree, Key root, const Visit& visit) {
    struct At {
        std::uint8_t depth = 0;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t z = 0;
    };
    std::vector<At> pending{{}};
    while (!pending.empty()) {
        const At at = pending.back();
        pending.pop_back();
        const auto found = tree.at(at.depth, at.x, at.y, at.z);
        const bool leaf = at.depth == tree.levels() ||
                          tree.at(at.depth + 1, 2 * at.x, 2 * at.y, 2 * at.z).depth == at.depth;
        if (!leaf) {
            for (std::uint32_t octant = 0; octant < 8; ++octant) {
                pending.push_back({.depth = static_cast<std::uint8_t>(at.depth + 1),
                                   .x = (2 * at.x) + (octant & 1U),
                                   .y = (2 * at.y) + ((octant >> 1U) & 1U),
                                   .z = (2 * at.z) + ((octant >> 2U) & 1U)});
            }
            continue;
        }
        if (found.open) {
            visit(Key{.depth = static_cast<std::uint8_t>(root.depth + at.depth),
                      .x = (root.x << at.depth) + at.x,
                      .y = (root.y << at.depth) + at.y,
                      .z = (root.z << at.depth) + at.z},
                  found);
        }
    }
}

} // namespace hog::grid
