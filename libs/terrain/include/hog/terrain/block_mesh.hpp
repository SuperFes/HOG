#pragma once

#include <hog/core/math.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace hog::terrain {

// A corner of a surface, textured by the tiles of the triangle it's drawn in.
struct BlockVertex {
    Vec3 position{0.0F}; // metres from the mesh's origin
    Vec3 normal{0.0F};   // unit, planet-centred, out of the side
    // The tiles at the triangle's three corners, and which of them this corner is, as
    // packed_corner has it with how wet the ground is there. A vertex shared between triangles
    // names its own tile three times, so whichever triangle draws it finds it.
    std::array<std::uint8_t, 3> tiles{};
    std::uint8_t corner = 0;

    friend bool operator==(const BlockVertex&, const BlockVertex&) = default;
};

// A vertex's corner byte: which corner of its triangle it is, in the low two bits, and in the six
// over them either how wet the ground is there, from dry at 0 to soaked at 47, 48 where water
// stands on it, or, from 49 to 63, how hot it shines; hot ground is dry, so the one field serves
// all.
inline constexpr unsigned soaked_level = 47;
inline constexpr unsigned standing_level = 48;
inline constexpr unsigned glow_base = 48;
inline constexpr unsigned glow_levels = 15;
[[nodiscard]] constexpr std::uint8_t packed_corner(std::uint8_t which, double wet,
                                                   double glow = 0.0,
                                                   bool standing = false) noexcept {
    unsigned level = standing ? standing_level
                              : static_cast<unsigned>(std::clamp(wet, 0.0, 1.0) * soaked_level);
    if (glow > 0.0) {
        level = glow_base + std::max(1U, static_cast<unsigned>(std::clamp(glow, 0.0, 1.0) *
                                                               glow_levels));
    }
    return static_cast<std::uint8_t>((level << 2U) | (which & 3U));
}
[[nodiscard]] constexpr std::uint8_t corner_of(std::uint8_t packed) noexcept {
    return static_cast<std::uint8_t>(packed & 3U);
}
[[nodiscard]] constexpr double wetness_of(std::uint8_t packed) noexcept {
    const unsigned level = packed >> 2U;
    if (level == standing_level) {
        return 1.0;
    }
    return level <= soaked_level ? level / static_cast<double>(soaked_level) : 0.0;
}
[[nodiscard]] constexpr bool standing_of(std::uint8_t packed) noexcept {
    const unsigned level = packed >> 2U;
    return level == standing_level;
}
[[nodiscard]] constexpr double glow_of(std::uint8_t packed) noexcept {
    const unsigned level = packed >> 2U;
    return level > glow_base ? (level - glow_base) / static_cast<double>(glow_levels) : 0.0;
}
// The same vertex as the corner `which` of its triangle.
[[nodiscard]] constexpr std::uint8_t at_corner(std::uint8_t packed, std::uint8_t which) noexcept {
    return static_cast<std::uint8_t>((packed & ~3U) | (which & 3U));
}

// Triangles of three indices each, anticlockwise seen from outside.
struct BlockMesh {
    WorldPosition origin;
    std::vector<BlockVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// Gives each triangle whose corners wear different tiles three corners of its own, naming all
// three tiles, so they blend across it. Every vertex must name one tile three times.
void mix_corners(BlockMesh& mesh);

} // namespace hog::terrain
