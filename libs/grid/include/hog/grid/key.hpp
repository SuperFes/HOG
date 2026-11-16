#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hog::grid {

// Deepest a key goes: three bits a level and a marker bit fill 64.
inline constexpr std::uint8_t max_depth = 21;

// A node of the grid octree: `depth` halvings below the root, at (x, y, z) among the 2^depth
// nodes a side at that depth, counted from the root's low corner.
struct Key {
    std::uint8_t depth = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;

    // Nodes a side at its depth.
    [[nodiscard]] constexpr std::uint32_t across() const noexcept {
        return std::uint32_t{1} << depth;
    }
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return depth <= max_depth && x < across() && y < across() && z < across();
    }

    // Precondition: depth > 0.
    [[nodiscard]] constexpr Key parent() const noexcept {
        return {.depth = static_cast<std::uint8_t>(depth - 1U), .x = x / 2, .y = y / 2, .z = z / 2};
    }
    // Which of its parent's eight this is: bits 1 along x, 2 along y and 4 along z.
    [[nodiscard]] constexpr std::size_t octant() const noexcept {
        return (x % 2) + (2 * (y % 2)) + (4 * (z % 2));
    }
    // Precondition: depth < max_depth.
    [[nodiscard]] constexpr Key child(std::size_t octant) const noexcept {
        return {.depth = static_cast<std::uint8_t>(depth + 1U),
                .x = (2 * x) + static_cast<std::uint32_t>(octant & 1U),
                .y = (2 * y) + static_cast<std::uint32_t>((octant >> 1U) & 1U),
                .z = (2 * z) + static_cast<std::uint32_t>((octant >> 2U) & 1U)};
    }
    // The node at `up_to` holding this one. Precondition: up_to <= depth.
    [[nodiscard]] constexpr Key ancestor(std::uint8_t up_to) const noexcept {
        const auto shift = static_cast<unsigned>(depth - up_to);
        return {.depth = up_to, .x = x >> shift, .y = y >> shift, .z = z >> shift};
    }
    // Whether `other` lies within this node, or is it.
    [[nodiscard]] constexpr bool contains(Key other) const noexcept {
        return other.depth >= depth && other.ancestor(depth) == *this;
    }
    // The node at the same depth `dx`, `dy`, `dz` away; none past the root's sides.
    [[nodiscard]] constexpr std::optional<Key> beside(int dx, int dy, int dz) const noexcept {
        const auto step = [this](std::uint32_t at, int by) -> std::optional<std::uint32_t> {
            const std::int64_t moved = std::int64_t{at} + by;
            if (moved < 0 || moved >= std::int64_t{across()}) {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(moved);
        };
        const auto nx = step(x, dx);
        const auto ny = step(y, dy);
        const auto nz = step(z, dz);
        if (!nx || !ny || !nz) {
            return std::nullopt;
        }
        return Key{.depth = depth, .x = *nx, .y = *ny, .z = *nz};
    }

    friend constexpr auto operator<=>(const Key&, const Key&) noexcept = default;
};

// The low 21 bits of `value` spread to every third bit.
[[nodiscard]] constexpr std::uint64_t spread(std::uint32_t value) noexcept {
    std::uint64_t bits = value & 0x1F'FFFFU;
    bits = (bits | (bits << 32U)) & 0x1F'0000'0000'FFFFULL;
    bits = (bits | (bits << 16U)) & 0x1F'0000'FF00'00FFULL;
    bits = (bits | (bits << 8U)) & 0x100F'00F0'0F00'F00FULL;
    bits = (bits | (bits << 4U)) & 0x10C3'0C30'C30C'30C3ULL;
    bits = (bits | (bits << 2U)) & 0x1249'2492'4924'9249ULL;
    return bits;
}

// Every third bit of `bits` gathered into the low 21.
[[nodiscard]] constexpr std::uint32_t gather(std::uint64_t bits) noexcept {
    bits &= 0x1249'2492'4924'9249ULL;
    bits = (bits | (bits >> 2U)) & 0x10C3'0C30'C30C'30C3ULL;
    bits = (bits | (bits >> 4U)) & 0x100F'00F0'0F00'F00FULL;
    bits = (bits | (bits >> 8U)) & 0x1F'0000'FF00'00FFULL;
    bits = (bits | (bits >> 16U)) & 0x1F'0000'0000'FFFFULL;
    bits = (bits | (bits >> 32U)) & 0x1F'FFFFULL;
    return static_cast<std::uint32_t>(bits);
}

// x, y and z's bits interleaved, x lowest: the nodes of one depth in Z order, each node's eight
// children consecutive.
[[nodiscard]] constexpr std::uint64_t morton(Key key) noexcept {
    return spread(key.x) | (spread(key.y) << 1U) | (spread(key.z) << 2U);
}

// One number for a node of any depth: a marker bit over its Morton bits, so a node's code is its
// child's shifted down three and the root's is 1. Precondition: a valid key.
[[nodiscard]] constexpr std::uint64_t code(Key key) noexcept {
    return (std::uint64_t{1} << (3U * key.depth)) | morton(key);
}

// Precondition: a code of a valid key.
[[nodiscard]] constexpr Key from_code(std::uint64_t code) noexcept {
    std::uint8_t depth = 0;
    while (depth < max_depth && (code >> (3U * (depth + 1U))) != 0) {
        ++depth;
    }
    const std::uint64_t bits = code & ((std::uint64_t{1} << (3U * depth)) - 1U);
    return {.depth = depth, .x = gather(bits), .y = gather(bits >> 1U), .z = gather(bits >> 2U)};
}

} // namespace hog::grid
