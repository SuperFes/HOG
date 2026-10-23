#pragma once

#include <hog/planet/cell_id.hpp>
#include <hog/planet/neighbors.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>

namespace hog::terrain::detail {

// Leaves sorted by cell, which don't overlap.
using Leaves = std::span<const planet::CellId>;

// The leaf covering all of `cell`, if one does rather than several finer ones.
[[nodiscard]] inline std::optional<planet::CellId> leaf_over(Leaves leaves, planet::CellId cell) {
    // Leaves don't overlap, so the one covering `cell` sorts next to it.
    const auto after = std::ranges::lower_bound(leaves, cell);
    if (after != leaves.end() && after->contains(cell)) {
        return *after;
    }
    if (after != leaves.begin() && std::prev(after)->contains(cell)) {
        return *std::prev(after);
    }
    return std::nullopt;
}

// Whether any leaf lies within `cell`.
[[nodiscard]] inline bool any_within(Leaves leaves, planet::CellId cell) {
    const auto within = std::ranges::lower_bound(leaves, cell.range_min());
    return within != leaves.end() && *within <= cell.range_max();
}

// Edge bits of a leaf, where the leaf beside it is coarser.
[[nodiscard]] inline std::uint8_t coarser_edges(Leaves leaves, planet::CellId cell) {
    std::uint8_t coarser = 0;
    const auto neighbours = planet::edge_neighbors(cell);
    for (std::size_t edge = 0; edge < neighbours.size(); ++edge) {
        const auto over = leaf_over(leaves, neighbours.at(edge));
        if (over && over->level() < cell.level()) {
            coarser |= static_cast<std::uint8_t>(1U << edge);
        }
    }
    return coarser;
}

} // namespace hog::terrain::detail
