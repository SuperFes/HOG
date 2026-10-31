#pragma once

#include <hog/core/math.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <span>

namespace hog::globe {

// Splits along each icosahedron edge are 2^level, so the finest cells of an Earth-sized globe are
// under a metre across.
inline constexpr int max_level = 24;

// A column of the globe: a vertex of an icosahedron whose triangles are split 2^level times along
// each edge, projected onto the sphere. Its cell is the patch of sphere nearer it than any other
// vertex: a hexagon, or a pentagon at the icosahedron's twelve corners.
//
// The icosahedron is cut into ten diamonds of two triangles, five about each pole; a cell is
// (i, j) within one, or one of the poles, which no diamond holds.
struct HexCell {
    static constexpr std::uint8_t north_pole = 10;
    static constexpr std::uint8_t south_pole = 11;

    std::uint8_t level = 0;
    std::uint8_t diamond = 0; // 0–4 about the north pole, 5–9 about the south, or a pole
    std::uint32_t i = 0;
    std::uint32_t j = 0;

    [[nodiscard]] static constexpr HexCell pole(int level, bool north) {
        return {.level = static_cast<std::uint8_t>(level),
                .diamond = north ? north_pole : south_pole};
    }

    [[nodiscard]] constexpr bool is_pole() const noexcept { return diamond >= north_pole; }
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        if (level > max_level || diamond > south_pole) {
            return false;
        }
        const std::uint32_t side = std::uint32_t{1} << level;
        return is_pole() ? i == 0 && j == 0 : i < side && j < side;
    }

    friend constexpr auto operator<=>(const HexCell&, const HexCell&) noexcept = default;
};

// Cells at a level: ten diamonds of 4^level and the poles.
[[nodiscard]] constexpr std::uint64_t cell_count(int level) noexcept {
    return (std::uint64_t{10} << (2 * level)) + 2;
}

// A cell's place in its level, from 0 to cell_count(level), and back.
[[nodiscard]] std::uint64_t index_of(HexCell cell);
[[nodiscard]] HexCell cell_at_index(int level, std::uint64_t index);

// Five sides at the icosahedron's corners, six everywhere else.
[[nodiscard]] bool is_pentagon(HexCell cell);

// The unit direction to the cell's centre.
[[nodiscard]] DVec3 center(HexCell cell);

// The cell at `level` whose centre is nearest the unit `direction`.
[[nodiscard]] HexCell cell_at(DVec3 direction, int level);

struct HexNeighbors {
    std::array<HexCell, 6> cells{};
    std::uint8_t count = 0;

    [[nodiscard]] std::span<const HexCell> all() const& noexcept { return {cells.data(), count}; }
    // A span into a temporary would outlive it.
    [[nodiscard]] std::span<const HexCell> all() const&& = delete;
};

// The cells sharing a side with `cell`, found exactly across diamonds and about the corners.
[[nodiscard]] HexNeighbors neighbors(HexCell cell);

} // namespace hog::globe
