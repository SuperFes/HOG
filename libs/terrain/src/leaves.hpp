#pragma once

#include <hog/planet/cell_id.hpp>
#include <hog/planet/neighbors.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>

namespace hog::terrain::detail {

// The leaf covering all of `cell`, if one does rather than several finer ones.
[[nodiscard]] inline std::optional<planet::CellId> leaf_over(const std::set<planet::CellId>& leaves,
                                                             planet::CellId cell) {
    // Leaves don't overlap, so the one covering `cell` sorts next to it.
    const auto after = leaves.lower_bound(cell);
    if (after != leaves.end() && after->contains(cell)) {
        return *after;
    }
    if (after != leaves.begin() && std::prev(after)->contains(cell)) {
        return *std::prev(after);
    }
    return std::nullopt;
}

// Edge bits of a leaf, where the leaf beside it is coarser.
[[nodiscard]] inline std::uint8_t coarser_edges(const std::set<planet::CellId>& leaves,
                                                planet::CellId cell) {
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
