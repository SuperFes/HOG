#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace hog::baked {

// A place on one of the planet's six faces, counted in the finest voxels: (x, y) the cell of the
// layers' level, z the layer.
struct Point {
    std::uint8_t face = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;

    friend constexpr auto operator<=>(const Point&, const Point&) noexcept = default;
};

// A cube of a face's octree: 2^size finest voxels a side from its corner, which lies on that
// size's grid.
struct Node {
    std::uint8_t face = 0;
    std::uint8_t size = 0; // log2 of its side
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;

    // Finest voxels across.
    [[nodiscard]] constexpr std::uint64_t side() const noexcept { return std::uint64_t{1} << size; }
    [[nodiscard]] constexpr bool contains(Point p) const noexcept {
        // Below the corner wraps round to far past the side.
        return p.face == face && std::uint64_t{p.x - x} < side() &&
               std::uint64_t{p.y - y} < side() && std::uint64_t{p.z - z} < side();
    }
    [[nodiscard]] constexpr bool contains(const Node& other) const noexcept {
        return other.size <= size &&
               contains(Point{.face = other.face, .x = other.x, .y = other.y, .z = other.z});
    }
    // Which of its eight halves: bits 1 along x, 2 along y and 4 along z. Precondition: size > 0.
    [[nodiscard]] constexpr Node child(std::size_t octant) const noexcept {
        const std::uint32_t half = std::uint32_t{1} << (size - 1U);
        return {.face = face,
                .size = static_cast<std::uint8_t>(size - 1U),
                .x = x + ((octant & 1U) != 0 ? half : 0U),
                .y = y + ((octant & 2U) != 0 ? half : 0U),
                .z = z + ((octant & 4U) != 0 ? half : 0U)};
    }
    // The one of `size` holding a point.
    [[nodiscard]] static constexpr Node holding(Point p, std::uint8_t size) noexcept {
        const std::uint32_t mask = ~((std::uint32_t{1} << size) - 1U);
        return {.face = p.face, .size = size, .x = p.x & mask, .y = p.y & mask, .z = p.z & mask};
    }

    friend constexpr auto operator<=>(const Node&, const Node&) noexcept = default;
};

// Coordinates up to this many bits each fit a key: a face 2^24 cells across, as `Cell` allows.
inline constexpr int coordinate_bits = 24;

// A node as a key: its face, then its corner's Morton code, then its size, all big-endian, so
// keys sort as nodes do along the Z-order curve and every node's keys lie between its corner's and
// its last voxel's. Nodes that never overlap therefore sort by place, and the node holding a
// point is the greatest key no greater than the point's own.
inline constexpr std::size_t key_bytes = 11;
using Key = std::array<std::byte, key_bytes>;

[[nodiscard]] Key key_of(const Node& node) noexcept;
// The greatest key any node holding `p` can have: its corner's code with the largest size.
[[nodiscard]] Key key_at(Point p) noexcept;
// The node of a key; none for bytes that aren't one.
[[nodiscard]] std::optional<Node> node_of(std::span<const std::byte> key) noexcept;

// The greatest key of any node within `node`: its last voxel's code with the largest size.
[[nodiscard]] Key last_key_in(const Node& node) noexcept;

} // namespace hog::baked
