#pragma once

#include <hog/planet/cell_id.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace hog::planet {

// The four cells sharing an edge with `cell`, at the same level, in the order -u, +u, -v, +v
// (i - 1, i + 1, j - 1, j + 1), crossing onto neighbouring faces where needed.
[[nodiscard]] std::array<CellId, 4> edge_neighbors(CellId cell);

// Cells sharing an edge or a corner with a cell, at the same level. Usually eight; seven for
// cells touching a cube corner, where only three faces meet, and four for a face cell.
class VertexNeighbors {
public:
    [[nodiscard]] std::span<const CellId> cells() const noexcept { return {cells_.data(), size_}; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    void push_back(CellId cell);

private:
    std::array<CellId, 8> cells_{};
    std::size_t size_ = 0;
};

[[nodiscard]] VertexNeighbors vertex_neighbors(CellId cell);

} // namespace hog::planet
