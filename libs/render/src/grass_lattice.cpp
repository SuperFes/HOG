#include <hog/flora/stand.hpp>
#include <hog/planet/projection.hpp>
#include <hog/render/grass_lattice.hpp>

namespace hog::render {

namespace {

// The child of a cell its dice pick: i in the low bit, j in the high.
std::uint32_t picked_child(std::uint32_t hash) noexcept {
    return hash >> 30U;
}

} // namespace

int grass_lattice_level(double planet_radius) {
    return planet::level_for(finest_grass_cell, planet_radius);
}

GrassLattice grass_lattice(double planet_radius, std::uint64_t world_seed) {
    return {.level = grass_lattice_level(planet_radius),
            .seed = static_cast<std::uint32_t>(world_seed ^ (world_seed >> 32U))};
}

std::uint32_t grass_hash(planet::FaceIJ cell, std::uint32_t seed) noexcept {
    const auto where =
        static_cast<std::uint32_t>(cell.level) | (static_cast<std::uint32_t>(cell.face) << 8U);
    return flora::hashed((cell.i * 0x8da6'b343U) ^
                         flora::hashed((cell.j * 0xd816'3841U) ^ flora::hashed(seed ^ where)));
}

planet::FaceIJ standing_for(planet::FaceIJ cell, int lattice_level, std::uint32_t seed) noexcept {
    while (cell.level < lattice_level) {
        const std::uint32_t child = picked_child(grass_hash(cell, seed));
        cell = {.face = cell.face,
                .i = (cell.i << 1U) | (child & 1U),
                .j = (cell.j << 1U) | (child >> 1U),
                .level = cell.level + 1};
    }
    return cell;
}

int stands_up(planet::FaceIJ cell, std::uint32_t seed, int most) noexcept {
    int up = 0;
    while (up < most && cell.level > 0) {
        const std::uint32_t child = (cell.i & 1U) | ((cell.j & 1U) << 1U);
        cell = {.face = cell.face, .i = cell.i >> 1U, .j = cell.j >> 1U, .level = cell.level - 1};
        if (picked_child(grass_hash(cell, seed)) != child) {
            break;
        }
        ++up;
    }
    return up;
}

Vec2 grass_spot(std::uint32_t hash) noexcept {
    const std::uint32_t spot = flora::hashed(hash);
    return {static_cast<float>(spot & 0xffffU) / 65536.0F,
            static_cast<float>(spot >> 16U) / 65536.0F};
}

} // namespace hog::render
