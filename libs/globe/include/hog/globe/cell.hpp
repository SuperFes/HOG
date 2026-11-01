#pragma once

#include <hog/core/math.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>

namespace hog::globe {

// A column of the cube-sphere: the planet as a cube inflated round, each of its six faces a square
// grid of `2^level` cells a side, warped onto the sphere by the tangent mapping so a cell's area
// varies by at most √2 between a face's middle and its corners. Face f looks along axis f % 3,
// outward for f < 3 and inward after; (i, j) run along its own two axes, right-handed seen from
// outside.
struct Cell {
    static constexpr int max_level = 24;

    std::uint8_t level = 0;
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        if (level > max_level || face >= 6) {
            return false;
        }
        const std::uint32_t side = std::uint32_t{1} << level;
        return i < side && j < side;
    }

    friend constexpr auto operator<=>(const Cell&, const Cell&) noexcept = default;
};

[[nodiscard]] constexpr std::uint64_t cells_at(int level) noexcept {
    return std::uint64_t{6} << (2 * level);
}

// A cell's place in its level, from 0 to cells_at(level), and back.
[[nodiscard]] std::uint64_t index_of(Cell cell);
[[nodiscard]] Cell cell_at_index(int level, std::uint64_t index);

// The unit direction through a cell's middle, and the cell a direction passes through.
[[nodiscard]] DVec3 center(Cell cell);
[[nodiscard]] Cell cell_under(DVec3 direction, int level);

// Its corners as unit directions, anticlockwise seen from outside. Its sides are great-circle
// arcs, so a neighbour shares two of them exactly.
[[nodiscard]] std::array<DVec3, 4> corners(Cell cell);

// Steradians it covers, and the volume over it between two radii.
[[nodiscard]] double solid_angle(Cell cell);
[[nodiscard]] double prism_volume(Cell cell, double inner, double outer);

struct CellNeighbors {
    std::array<Cell, 8> cells{};
    std::uint8_t count = 0;

    [[nodiscard]] std::span<const Cell> all() const& noexcept { return {cells.data(), count}; }
    [[nodiscard]] std::span<const Cell> all() const&& = delete;
};

// The four cells across its sides, the next face's where it lies on the cube's edge; or those
// `apart` cells on, straight on over the cube's edges, less than a face across.
[[nodiscard]] CellNeighbors sides(Cell cell, int apart = 1);
// Those, and the cells it meets only at a corner: eight in all, or seven at the cube's corners,
// where three cells meet instead of four.
[[nodiscard]] CellNeighbors around(Cell cell);

// The cell `di` along i and `dj` along j from it, each less than a face across, straight on over
// onto the next face where it steps off this one; none where both steps leave the face, past the
// cube's corner.
[[nodiscard]] std::optional<Cell> next_to(Cell cell, int di, int dj);

// The four cells a level finer that tile it exactly, and the one a level coarser it lies in.
[[nodiscard]] std::array<Cell, 4> children(Cell cell);
[[nodiscard]] Cell parent(Cell cell);

} // namespace hog::globe
