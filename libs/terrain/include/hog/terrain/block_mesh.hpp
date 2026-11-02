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

// A vertex's corner byte: which corner of its triangle it is, in the low two bits, and how wet
// the ground is there in the six over them, from dry at 0 to soaked at 63, to the 63rd under.
[[nodiscard]] constexpr std::uint8_t packed_corner(std::uint8_t which, double wet) noexcept {
    const auto soaked = static_cast<unsigned>(std::clamp(wet, 0.0, 1.0) * 63.0);
    return static_cast<std::uint8_t>((soaked << 2U) | (which & 3U));
}
[[nodiscard]] constexpr std::uint8_t corner_of(std::uint8_t packed) noexcept {
    return static_cast<std::uint8_t>(packed & 3U);
}
[[nodiscard]] constexpr double wetness_of(std::uint8_t packed) noexcept {
    return (packed >> 2U) / 63.0;
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
