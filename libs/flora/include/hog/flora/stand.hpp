#pragma once

#include <hog/core/math.hpp>
#include <hog/flora/species.hpp>
#include <hog/planet/climate.hpp>

#include <cstdint>
#include <optional>

namespace hog::flora {

// Where trees stand: each level-14 lattice cell rolls its own dice, gathered into woods by the
// stand field. This is the one placement, shared by whoever plants — the client's anchors today,
// the server's felling and regrowth tomorrow — so it is bit-stable across machines, like grow().

// Each biome's trees against the stand field: how deep the field must read before woods begin
// (`edge`, 1 grows none), how full the woods are inside, the odd lone tree in the open, and how
// many are oaks, gathered in groves among the pines. Bushes read the same table, crowding the
// woods' edges and thinning under the canopy.
struct BiomeTrees {
    float height = 0.0F;    // metres; none grows no trees
    float inside = 0.0F;    // of lattice cells grown within a stand, 0 to 1
    float edge = 1.0F;      // the stand field's woodland threshold
    float lone = 0.0F;      // of lattice cells grown in the open
    float broadleaf = 0.0F; // of the trees grown oaks, about
};

[[nodiscard]] constexpr BiomeTrees trees_of(planet::Biome biome) noexcept {
    using enum planet::Biome;
    switch (biome) {
    case BorealForest:
        return {.height = 13.0F, .inside = 0.8F, .edge = 0.42F, .lone = 0.015F};
    case TemperateForest:
        return {.height = 15.0F, .inside = 0.75F, .edge = 0.5F, .lone = 0.012F, .broadleaf = 0.55F};
    case Grassland:
        return {.height = 12.0F, .inside = 0.6F, .edge = 0.66F, .lone = 0.0015F, .broadleaf = 0.7F};
    case Shrubland:
        return {.height = 7.0F, .inside = 0.5F, .edge = 0.62F, .lone = 0.004F, .broadleaf = 0.35F};
    case Alpine:
        return {.height = 4.5F, .inside = 0.5F, .edge = 0.6F, .lone = 0.008F};
    case Wetland:
        return {.height = 10.0F, .inside = 0.55F, .edge = 0.58F, .lone = 0.008F, .broadleaf = 0.3F};
    case Tundra:
    case Desert:
    case Savanna:
    case TropicalRainforest: // its palms are a species to come
    case Ocean:
    case Beach:
    case Ice:
    case Lake:
        return {};
    }
    return {};
}

// Foliage far darker than the ground's mean, so a forest reads as its own mass: needles cool and
// deep, leaves warmer and brighter.
[[nodiscard]] constexpr Vec3 foliage_colour(Genus genus, Vec3 ground) noexcept {
    if (genus == Genus::Oak) {
        return (ground * 0.38F) + (Vec3{0.022F, 0.062F, 0.012F});
    }
    return (ground * 0.3F) + (Vec3{0.012F, 0.05F, 0.028F});
}

// The dice a lattice cell rolls, the same from every patch and machine over it.
[[nodiscard]] std::uint32_t hashed(std::uint32_t at) noexcept;
[[nodiscard]] float unit(std::uint32_t at) noexcept;
[[nodiscard]] std::uint32_t cell_hash(std::uint32_t lattice_i, std::uint32_t lattice_j,
                                      std::uint32_t face) noexcept;

// How deep into woodland a spot lies, 0 to 1, over `cell` given in the trees' lattice: broad
// stands with clearings between, edged by a finer grain.
[[nodiscard]] float stand_field(Vec2 cell, std::uint32_t face) noexcept;

// A tree stood in its lattice cell: what kind, where within the cell, how old, and how tall for
// its age and the stand around it.
struct Stood {
    Vec2 spot;           // 0 to 1 across the cell, where the trunk roots
    float height = 0.0F; // metres
    float wooded = 0.0F; // how deep in a stand it grows, 0 to 1
    float age = 0.0F;    // 0 a sapling, 1 full grown
    std::uint32_t hash = 0;
    Genus genus = Genus::Pine;
};

// Where within its cell the cell's tree would root — always answered, so the ground and biome
// there can be sampled before tree_grown() decides whether one grows at all.
[[nodiscard]] Vec2 tree_spot(std::uint32_t lattice_i, std::uint32_t lattice_j,
                             std::uint32_t face) noexcept;

// The tree the cell grows on `biome` ground, if its dice land one there.
[[nodiscard]] std::optional<Stood> tree_grown(std::uint32_t lattice_i, std::uint32_t lattice_j,
                                              std::uint32_t face, planet::Biome biome) noexcept;

} // namespace hog::flora
