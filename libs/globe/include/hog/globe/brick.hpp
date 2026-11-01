#pragma once

#include <hog/core/bytes.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hog::globe {

// Voxels are held in cubes of 8³: whole bricks are what's stored, saved and sent.
inline constexpr int brick_side = 8;
inline constexpr int brick_voxels = brick_side * brick_side * brick_side;

// A brick's place, counted in bricks.
struct BrickSite {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    std::uint32_t layer = 0;

    friend constexpr auto operator<=>(const BrickSite&, const BrickSite&) noexcept = default;
};

[[nodiscard]] constexpr BrickSite brick_of(Site site) noexcept {
    return {.face = site.face,
            .i = site.i / brick_side,
            .j = site.j / brick_side,
            .layer = site.layer / brick_side};
}

// A brick's site as one number, ordered by face, then i, then j, then layer: the face in 3 bits,
// i and j in 21 each, the layer in 19.
inline constexpr std::uint32_t most_bricks_up = std::uint32_t{1} << 19U;

[[nodiscard]] constexpr std::uint64_t brick_key(BrickSite site) noexcept {
    return (std::uint64_t{site.face} << 61U) | (std::uint64_t{site.i} << 40U) |
           (std::uint64_t{site.j} << 19U) | site.layer;
}

[[nodiscard]] constexpr BrickSite brick_at_key(std::uint64_t key) noexcept {
    constexpr std::uint64_t across = (std::uint64_t{1} << 21U) - 1;
    return {.face = static_cast<std::uint8_t>(key >> 61U),
            .i = static_cast<std::uint32_t>((key >> 40U) & across),
            .j = static_cast<std::uint32_t>((key >> 19U) & across),
            .layer = static_cast<std::uint32_t>(key & (most_bricks_up - 1))};
}

// The brick's voxel at `index`, which runs along i, then j, then up.
[[nodiscard]] constexpr Site site_in(BrickSite brick, int index) noexcept {
    const auto at = static_cast<std::uint32_t>(index);
    return {.face = brick.face,
            .i = (brick.i * brick_side) + (at % brick_side),
            .j = (brick.j * brick_side) + ((at / brick_side) % brick_side),
            .layer = (brick.layer * brick_side) + (at / (brick_side * brick_side))};
}

[[nodiscard]] constexpr int index_in_brick(Site site) noexcept {
    const std::uint32_t row = ((site.layer % brick_side) * brick_side) + (site.j % brick_side);
    return static_cast<int>((row * brick_side) + (site.i % brick_side));
}

// One quantity over a brick's voxels in as few bits as tell its values apart: one value for all,
// a palette and indices into it, or every value in full when that is smaller.
template <typename T>
class Packed {
public:
    [[nodiscard]] static Packed pack(std::span<const T, brick_voxels> values);

    [[nodiscard]] T at(int index) const noexcept;
    void unpack(std::span<T, brick_voxels> into) const noexcept;

    [[nodiscard]] bool is_uniform() const noexcept { return values_.empty(); }
    // Held on the heap.
    [[nodiscard]] std::size_t bytes() const noexcept;

    void write(ByteWriter& out) const;
    [[nodiscard]] static Result<Packed> read(ByteReader& in);

    friend bool operator==(const Packed&, const Packed&) = default;

private:
    T one_{};
    std::vector<T> values_; // the palette, or every value in full
    std::vector<std::uint64_t> words_;
    std::uint8_t bits_ = 0; // per index into the palette
};

// A brick's voxels: one voxel throughout, as is common where a brick is all dug out or filled,
// or each quantity packed apart, so a brick whose distances all differ still keeps a few bits a
// voxel for what it's made of.
class Brick {
public:
    Brick() = default;
    explicit Brick(Voxel whole) noexcept : whole_{whole} {}
    [[nodiscard]] static Brick pack(std::span<const Voxel, brick_voxels> voxels);

    Brick(const Brick& other);
    Brick& operator=(const Brick& other);
    Brick(Brick&&) noexcept = default;
    Brick& operator=(Brick&&) noexcept = default;
    ~Brick() = default;

    [[nodiscard]] Voxel at(int index) const noexcept;
    void unpack(std::span<Voxel, brick_voxels> into) const noexcept;

    [[nodiscard]] bool is_uniform() const noexcept { return parts_ == nullptr; }
    // Held, the brick itself and what it points to.
    [[nodiscard]] std::size_t bytes() const noexcept;

    void write(ByteWriter& out) const;
    [[nodiscard]] static Result<Brick> read(ByteReader& in);

    friend bool operator==(const Brick& a, const Brick& b) noexcept;

private:
    struct Parts {
        Packed<std::int16_t> distance;
        Packed<Material> solid;
        Packed<std::uint8_t> condition;
        Packed<Material> grain;
        Packed<std::uint16_t> grain_amount;
        Packed<Material> liquid;
        Packed<std::uint16_t> liquid_amount;
        Packed<std::uint16_t> temperature;

        friend bool operator==(const Parts&, const Parts&) = default;
    };

    Voxel whole_;
    std::unique_ptr<Parts> parts_;
};

} // namespace hog::globe
