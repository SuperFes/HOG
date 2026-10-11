#pragma once

#include <hog/core/assert.hpp>

#include <bit>
#include <compare>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace hog::planet {

inline constexpr int face_count = 6;
inline constexpr int max_level = 30;

// A cell's face, level and integer position on that face; i and j are below 2^level.
struct FaceIJ {
    int face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    int level = 0;

    friend constexpr bool operator==(const FaceIJ&, const FaceIJ&) = default;
};

// One cell of the cube-sphere quadtree, packed into 64 bits:
//
//     face (3 bits) | position (2 bits per level) | 1 | zeros
//
// The trailing 1 marks the level, so a cell's descendants form one contiguous range of ids and
// parents and children are bit arithmetic. Positions follow a Morton curve; the curve is private
// to this header, so it can change (to Hilbert, say) without touching callers, though stored ids
// would need migrating. The raw value is stable and meant for storage.
class CellId {
public:
    constexpr CellId() noexcept = default;

    [[nodiscard]] static constexpr CellId from_raw(std::uint64_t raw) noexcept {
        return CellId{raw};
    }

    [[nodiscard]] static constexpr CellId from_face(int face) {
        return from_face_ij(face, 0, 0, 0);
    }

    [[nodiscard]] static constexpr CellId from_face_ij(int face, std::uint32_t i, std::uint32_t j,
                                                       int level) {
        HOG_ASSERT(face >= 0 && face < face_count, "face out of range");
        HOG_ASSERT(level >= 0 && level <= max_level, "level out of range");
        HOG_ASSERT(i >> level == 0 && j >> level == 0, "position out of range for the level");
        const auto shift = position_shift(level);
        return CellId{(static_cast<std::uint64_t>(face) << face_shift) |
                      (morton_encode(i, j) << shift) | (std::uint64_t{1} << (shift - 1))};
    }

    [[nodiscard]] static constexpr CellId from_face_ij(FaceIJ cell) {
        return from_face_ij(cell.face, cell.i, cell.j, cell.level);
    }

    [[nodiscard]] constexpr std::uint64_t raw() const noexcept { return raw_; }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        // The marker must sit on an even bit no higher than a face cell's.
        constexpr std::uint64_t marker_bits = 0x1555'5555'5555'5555;
        return (raw_ >> face_shift) < face_count && (lowest_bit() & marker_bits) != 0;
    }

    [[nodiscard]] constexpr int face() const {
        HOG_ASSERT(is_valid(), "invalid cell");
        return static_cast<int>(raw_ >> face_shift);
    }

    [[nodiscard]] constexpr int level() const {
        HOG_ASSERT(is_valid(), "invalid cell");
        return max_level - (std::countr_zero(raw_) / 2);
    }

    [[nodiscard]] constexpr bool is_face() const { return level() == 0; }
    [[nodiscard]] constexpr bool is_leaf() const { return level() == max_level; }

    [[nodiscard]] constexpr FaceIJ to_face_ij() const {
        const int cell_level = level();
        const auto shift = position_shift(cell_level);
        const auto position = (raw_ & ~(std::uint64_t{7} << face_shift)) >> shift;
        const auto [i, j] = morton_decode(position);
        return {.face = face(), .i = i, .j = j, .level = cell_level};
    }

    [[nodiscard]] constexpr CellId parent() const {
        HOG_ASSERT(!is_face(), "a face cell has no parent");
        const auto marker = lowest_bit() << 2;
        return CellId{(raw_ & ~(marker - 1)) | marker};
    }

    [[nodiscard]] constexpr CellId parent(int at_level) const {
        HOG_ASSERT(at_level >= 0 && at_level <= level(), "parent level out of range");
        const auto marker = std::uint64_t{1} << (position_shift(at_level) - 1);
        return CellId{(raw_ & ~(marker - 1)) | marker};
    }

    // Children are numbered 0..3 in curve order.
    [[nodiscard]] constexpr CellId child(int index) const {
        HOG_ASSERT(!is_leaf(), "a leaf cell has no children");
        HOG_ASSERT(index >= 0 && index < 4, "child index out of range");
        const auto marker = lowest_bit();
        return CellId{(raw_ - marker) +
                      (static_cast<std::uint64_t>((2 * index) + 1) * (marker >> 2))};
    }

    // Lowest and highest leaf ids inside this cell; every descendant's id lies between them.
    [[nodiscard]] constexpr CellId range_min() const noexcept {
        return CellId{raw_ - (lowest_bit() - 1)};
    }
    [[nodiscard]] constexpr CellId range_max() const noexcept {
        return CellId{raw_ + (lowest_bit() - 1)};
    }

    [[nodiscard]] constexpr bool contains(CellId other) const noexcept {
        return other.raw_ >= range_min().raw_ && other.raw_ <= range_max().raw_;
    }

    friend constexpr auto operator<=>(CellId, CellId) noexcept = default;

private:
    static constexpr int face_shift = 61;

    constexpr explicit CellId(std::uint64_t raw) noexcept : raw_(raw) {}

    [[nodiscard]] constexpr std::uint64_t lowest_bit() const noexcept { return raw_ & -raw_; }

    // Position bits sit directly below the face, followed by the marker bit.
    [[nodiscard]] static constexpr int position_shift(int level) noexcept {
        return face_shift - (2 * level);
    }

    // Spreads the low 32 bits of v onto the even bits.
    [[nodiscard]] static constexpr std::uint64_t spread_bits(std::uint32_t v) noexcept {
        std::uint64_t x = v;
        x = (x | (x << 16)) & 0x0000'FFFF'0000'FFFF;
        x = (x | (x << 8)) & 0x00FF'00FF'00FF'00FF;
        x = (x | (x << 4)) & 0x0F0F'0F0F'0F0F'0F0F;
        x = (x | (x << 2)) & 0x3333'3333'3333'3333;
        x = (x | (x << 1)) & 0x5555'5555'5555'5555;
        return x;
    }

    [[nodiscard]] static constexpr std::uint32_t gather_bits(std::uint64_t x) noexcept {
        x &= 0x5555'5555'5555'5555;
        x = (x | (x >> 1)) & 0x3333'3333'3333'3333;
        x = (x | (x >> 2)) & 0x0F0F'0F0F'0F0F'0F0F;
        x = (x | (x >> 4)) & 0x00FF'00FF'00FF'00FF;
        x = (x | (x >> 8)) & 0x0000'FFFF'0000'FFFF;
        x = (x | (x >> 16)) & 0x0000'0000'FFFF'FFFF;
        return static_cast<std::uint32_t>(x);
    }

    // i takes the high bit of each pair, so child index = (i_bit << 1) | j_bit.
    [[nodiscard]] static constexpr std::uint64_t morton_encode(std::uint32_t i,
                                                               std::uint32_t j) noexcept {
        return (spread_bits(i) << 1) | spread_bits(j);
    }

    struct IJ {
        std::uint32_t i;
        std::uint32_t j;
    };

    [[nodiscard]] static constexpr IJ morton_decode(std::uint64_t position) noexcept {
        return {.i = gather_bits(position >> 1), .j = gather_bits(position)};
    }

    std::uint64_t raw_ = 0;
};

// Face then one child digit per level, e.g. "3/0213"; "invalid" for an invalid id.
[[nodiscard]] std::string to_string(CellId cell);

} // namespace hog::planet

template <>
struct std::formatter<hog::planet::CellId> : std::formatter<std::string_view> {
    auto format(hog::planet::CellId cell, std::format_context& context) const {
        return std::formatter<std::string_view>::format(hog::planet::to_string(cell), context);
    }
};
