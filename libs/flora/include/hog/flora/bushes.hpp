#pragma once

#include <hog/core/math.hpp>
#include <hog/flora/fates.hpp>
#include <hog/planet/climate.hpp>

#include <cstdint>
#include <optional>

namespace hog::flora {

// Where bushes grow: each level-16 lattice cell rolls its own dice, as the trees' cells do, so a
// bush keeps its spot, kind and size at every terrain level and on every machine. Shared by
// whoever plants, so it is bit-stable like the trees' placement.

// The level of the planet's cells bushes are planted on, one at most to a cell.
inline constexpr int bush_lattice_level = 16;

// Bushes' silhouettes: a floofy blob, a spiky rosette, a twiggy skeleton, or a low mat.
enum class BushKind : std::uint8_t { Floofy, Spiky, Twiggy, Mat };

// Each biome's bushes: how tall, how many lattice cells grow one, and the mix of kinds.
struct BiomeBushes {
    float height = 0.0F; // metres; none grows no bushes
    float share = 0.0F;  // of lattice cells grown, 0 to 1
    float floofy = 0.0F; // kinds' shares; low mats take what's left
    float spiky = 0.0F;
    float twiggy = 0.0F;
};

[[nodiscard]] constexpr BiomeBushes bushes_of(planet::Biome biome) noexcept {
    using enum planet::Biome;
    switch (biome) {
    case Tundra:
        return {.height = 0.35F, .share = 0.3F, .floofy = 0.0F, .spiky = 0.0F, .twiggy = 0.3F};
    case BorealForest:
        return {.height = 0.7F, .share = 0.25F, .floofy = 0.4F, .spiky = 0.0F, .twiggy = 0.3F};
    case TemperateForest:
        return {.height = 0.9F, .share = 0.3F, .floofy = 0.5F, .spiky = 0.0F, .twiggy = 0.3F};
    case Grassland:
        return {.height = 0.9F, .share = 0.07F, .floofy = 0.6F, .spiky = 0.0F, .twiggy = 0.3F};
    case Shrubland:
        return {.height = 1.1F, .share = 0.55F, .floofy = 0.4F, .spiky = 0.15F, .twiggy = 0.25F};
    case Desert:
        return {.height = 0.8F, .share = 0.08F, .floofy = 0.0F, .spiky = 0.6F, .twiggy = 0.3F};
    case Savanna:
        return {.height = 1.2F, .share = 0.18F, .floofy = 0.5F, .spiky = 0.2F, .twiggy = 0.3F};
    case TropicalRainforest:
        return {.height = 1.3F, .share = 0.4F, .floofy = 0.7F, .spiky = 0.1F, .twiggy = 0.0F};
    case Alpine:
        return {.height = 0.25F, .share = 0.15F, .floofy = 0.0F, .spiky = 0.0F, .twiggy = 0.2F};
    case Wetland:
        return {.height = 0.8F, .share = 0.12F, .floofy = 0.5F, .spiky = 0.0F, .twiggy = 0.5F};
    case Ocean:
    case Beach:
    case Ice:
    case Lake:
        return {};
    }
    return {};
}

// Of the ground the fallen dead leave, the share shrubs take.
inline constexpr float shrubs_in_fallen = 0.45F;

// Where within its cell the cell's bush would root, 0 to 1 across it.
[[nodiscard]] Vec2 bush_spot(std::uint32_t lattice_i, std::uint32_t lattice_j,
                             std::uint32_t face) noexcept;

// Where the cell's bush looks for the biome it grows as, -1 to 1 each way of the biomes' blending
// reach about its spot: near another biome a bush may be one of its, the fewer the further in, as
// the ground's own colours fade into each other there.
[[nodiscard]] Vec2 bush_stray(std::uint32_t lattice_i, std::uint32_t lattice_j,
                              std::uint32_t face) noexcept;

// A bush grown in its lattice cell.
struct Shrub {
    Vec2 spot;           // 0 to 1 across the cell, where it roots
    float height = 0.0F; // metres
    float width = 0.0F;  // metres
    std::uint32_t hash = 0;
    BushKind kind = BushKind::Floofy;
};

// The bush the cell grows as a `biome` bush, its woods faring as `fate` has them, if its dice land
// one there. Bushes crowd the woods' sunny edges and thin under the canopy, crowd in where the
// dead have fallen, and grow on no ground a fire left black.
[[nodiscard]] std::optional<Shrub> bush_grown(std::uint32_t lattice_i, std::uint32_t lattice_j,
                                              std::uint32_t face, planet::Biome biome,
                                              const Fate& fate = Fate{}) noexcept;

} // namespace hog::flora
