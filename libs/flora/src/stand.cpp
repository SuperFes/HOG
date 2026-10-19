#include <hog/flora/stand.hpp>

#include <cstdint>
#include <optional>

namespace hog::flora {

namespace {

// a + t(b - a), spelled out: the ordinary rounding of each step is part of the contract.
float mixed(float a, float b, float t) noexcept {
    return a + (t * (b - a));
}

float smooth(float t) noexcept {
    return t * t * (3.0F - (2.0F * t));
}

float stand_corner(std::uint32_t base_x, std::uint32_t base_y, std::uint32_t face,
                   std::uint32_t salt) noexcept {
    return unit(hashed(base_x ^ (face * 0x9e3779b9U) ^ salt) + base_y);
}

// Value noise over the trees' lattice, cells gathered `period_log` levels up. The coordinates are
// never negative, so truncation is the floor.
float stand_octave(Vec2 cell, std::uint32_t face, std::uint32_t period_log,
                   std::uint32_t salt) noexcept {
    const float period = static_cast<float>(std::uint32_t{1} << period_log);
    const float at_x = cell.x / period;
    const float at_y = cell.y / period;
    const auto base_x = static_cast<std::uint32_t>(at_x);
    const auto base_y = static_cast<std::uint32_t>(at_y);
    const float fx = smooth(at_x - static_cast<float>(base_x));
    const float fy = smooth(at_y - static_cast<float>(base_y));
    const float c00 = stand_corner(base_x, base_y, face, salt);
    const float c10 = stand_corner(base_x + 1, base_y, face, salt);
    const float c01 = stand_corner(base_x, base_y + 1, face, salt);
    const float c11 = stand_corner(base_x + 1, base_y + 1, face, salt);
    return mixed(mixed(c00, c01, fy), mixed(c10, c11, fy), fx);
}

float saturated(float value) noexcept {
    if (value < 0.0F) {
        return 0.0F;
    }
    return value > 1.0F ? 1.0F : value;
}

float smoothstepped(float from, float to, float value) noexcept {
    return smooth(saturated((value - from) / (to - from)));
}

} // namespace

std::uint32_t hashed(std::uint32_t at) noexcept {
    at ^= at >> 16U;
    at *= 0x7feb352dU;
    at ^= at >> 15U;
    at *= 0x846ca68bU;
    at ^= at >> 16U;
    return at;
}

float unit(std::uint32_t at) noexcept {
    return static_cast<float>(hashed(at) & 0xffffU) / 65536.0F;
}

std::uint32_t cell_hash(std::uint32_t lattice_i, std::uint32_t lattice_j,
                        std::uint32_t face) noexcept {
    return hashed(hashed(lattice_i ^ (face * 0x9e3779b9U)) + lattice_j);
}

float stand_field(Vec2 cell, std::uint32_t face) noexcept {
    return (0.65F * stand_octave(cell, face, 6U, 0x51a9dU)) +
           (0.35F * stand_octave(cell, face, 4U, 0xb07a7U));
}

Vec2 tree_spot(std::uint32_t lattice_i, std::uint32_t lattice_j, std::uint32_t face) noexcept {
    const std::uint32_t hash = cell_hash(lattice_i, lattice_j, face);
    return {unit(hash), unit(hash + 1U)};
}

std::optional<Stood> tree_grown(std::uint32_t lattice_i, std::uint32_t lattice_j,
                                std::uint32_t face, planet::Biome biome) noexcept {
    const BiomePines pines = pines_of(biome);
    if (pines.height <= 0.0F) {
        return std::nullopt;
    }
    const std::uint32_t hash = cell_hash(lattice_i, lattice_j, face);
    const Vec2 spot{unit(hash), unit(hash + 1U)};
    const Vec2 cell{static_cast<float>(lattice_i) + spot.x, static_cast<float>(lattice_j) + spot.y};
    const float wooded = smoothstepped(pines.edge, pines.edge + 0.1F, stand_field(cell, face));
    const float occupancy = mixed(pines.lone, pines.inside, wooded);
    if (unit(hash + 2U) >= occupancy) {
        return std::nullopt;
    }
    // Tallest deep in a stand, shorter toward its edges and alone in the open.
    const float height =
        pines.height * (0.55F + (0.9F * unit(hash + 4U))) * mixed(0.8F, 1.05F, wooded);
    return Stood{.spot = spot, .height = height, .wooded = wooded, .hash = hash};
}

} // namespace hog::flora
