#pragma once

#include <hog/core/math.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::globe {

// Splits along each icosahedron edge are 2^level, so the finest cells of an Earth-sized globe are
// under a metre across.
inline constexpr int max_level = 24;

// A column of the globe: a vertex of an icosahedron whose triangles are split 2^level times along
// each edge. Its cell is the flat hexagon about it, or pentagon at the icosahedron's twelve
// corners, carried onto the sphere by a projection that keeps areas, so every hexagon holds as
// much of the globe as any other.
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

// The cell at `level` holding the unit `direction`.
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

// The cell's share of the sphere in steradians: every hexagon's is the same, a pentagon's five
// sixths of it.
[[nodiscard]] double solid_angle(HexCell cell);

// The volume over the cell between radii `inner` and `outer`.
[[nodiscard]] double prism_volume(HexCell cell, double inner, double outer);

// A cell's neighbours anticlockwise seen from outside, and its corners as unit directions: corner
// k lies between neighbours k and k + 1, so side k, from corner k - 1 to corner k, faces
// neighbour k.
struct HexRing {
    std::array<HexCell, 6> neighbors{};
    std::array<DVec3, 6> corners{};
    std::uint8_t count = 0;

    [[nodiscard]] std::span<const HexCell> around() const& noexcept {
        return {neighbors.data(), count};
    }
    [[nodiscard]] std::span<const HexCell> around() const&& = delete;
    [[nodiscard]] std::span<const DVec3> outline() const& noexcept {
        return {corners.data(), count};
    }
    [[nodiscard]] std::span<const DVec3> outline() const&& = delete;
};

[[nodiscard]] HexRing ring(HexCell cell);

// The cells within `rings` steps of `centre`, it first, nearer before farther.
[[nodiscard]] std::vector<HexCell> disc(HexCell centre, int rings);

// Cells sharing in another a level away, each by how many halves of itself.
struct HexShares {
    std::array<HexCell, 7> cells{};
    std::array<std::uint8_t, 7> halves{};
    std::uint8_t count = 0;

    [[nodiscard]] std::span<const HexCell> all() const& noexcept { return {cells.data(), count}; }
    [[nodiscard]] std::span<const HexCell> all() const&& = delete;
};

// The cells a level finer under `cell`: the one at its centre, whole, and the one halfway to each
// neighbour, half, as the neighbour holds the other half.
[[nodiscard]] HexShares children(HexCell cell);

// The cells a level coarser over `cell`: one whole, or two halves where it lies halfway between
// them.
[[nodiscard]] HexShares parents(HexCell cell);

// Whole units of `amount` dealt out over `shares` by their halves, the remainder a unit each from
// the first, so they add back up to it exactly.
[[nodiscard]] std::array<std::int64_t, 7> share_out(std::int64_t amount, const HexShares& shares);

} // namespace hog::globe
