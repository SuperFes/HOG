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

// The level of the planet's cells trees are planted on, one at most to a cell.
inline constexpr int lattice_level = 14;

// Each biome's trees against the stand field: how deep the field must read before woods begin
// (`edge`, 1 grows none), how full the woods are inside, the odd lone tree in the open, how many
// are palms, in groves of their own, and of the rest how many are broadleaves, gathered in groves
// among the pines, and of those how many are maples, in stands of their own among the oaks. Each
// kind then grows only as far as the climate suits it. Bushes read the same table, crowding the
// woods' edges and thinning under the canopy.
struct BiomeTrees {
    float height = 0.0F;    // metres; none grows no trees
    float inside = 0.0F;    // of lattice cells grown within a stand, 0 to 1
    float edge = 1.0F;      // the stand field's woodland threshold
    float lone = 0.0F;      // of lattice cells grown in the open
    float broadleaf = 0.0F; // of the trees not palms broadleaves, about
    float maple = 0.0F;     // of the broadleaves maples, about
    float palm = 0.0F;      // of the trees palms, about
};

[[nodiscard]] constexpr BiomeTrees trees_of(planet::Biome biome) noexcept {
    using enum planet::Biome;
    switch (biome) {
    case BorealForest:
        return {.height = 13.0F, .inside = 0.8F, .edge = 0.42F, .lone = 0.015F};
    case TemperateForest:
        return {.height = 15.0F,
                .inside = 0.75F,
                .edge = 0.5F,
                .lone = 0.012F,
                .broadleaf = 0.55F,
                .maple = 0.4F};
    case Grassland:
        return {.height = 12.0F,
                .inside = 0.6F,
                .edge = 0.66F,
                .lone = 0.0015F,
                .broadleaf = 0.7F,
                .maple = 0.2F};
    case Shrubland:
        return {.height = 7.0F,
                .inside = 0.5F,
                .edge = 0.62F,
                .lone = 0.004F,
                .broadleaf = 0.35F,
                .maple = 0.15F};
    case Alpine:
        return {.height = 4.5F, .inside = 0.5F, .edge = 0.6F, .lone = 0.008F};
    case Wetland:
        return {.height = 10.0F,
                .inside = 0.55F,
                .edge = 0.58F,
                .lone = 0.008F,
                .broadleaf = 0.3F,
                .maple = 0.6F};
    // Palms the length of a warm shore, none where it is too cold for them; the oaks stand in for
    // the tropics' own broadleaves.
    case Beach:
        return {.height = 14.0F, .inside = 0.45F, .edge = 0.5F, .lone = 0.03F, .palm = 1.0F};
    case Savanna:
        return {.height = 10.0F,
                .inside = 0.3F,
                .edge = 0.62F,
                .lone = 0.012F,
                .broadleaf = 1.0F,
                .palm = 0.3F};
    case TropicalRainforest:
        return {.height = 24.0F,
                .inside = 0.9F,
                .edge = 0.36F,
                .lone = 0.03F,
                .broadleaf = 1.0F,
                .palm = 0.3F};
    case Tundra:
    case Desert:
    case Ocean:
    case Ice:
    case Lake:
        return {};
    }
    return {};
}

// The mean annual temperatures a kind grows at, °C: fully from `coldest` to `warmest`, thinning to
// none `margin` past either. Frost keeps the palms to warm coasts and the tropics; cold sets the
// pines' treeline, and heat ends them where the broadleaves take over.
struct Range {
    float coldest = 0.0F;
    float warmest = 0.0F;
    float margin = 1.0F;
};

[[nodiscard]] constexpr Range range_of(Genus genus) noexcept {
    switch (genus) {
    case Genus::Oak:
        return {.coldest = 4.0F, .warmest = 30.0F, .margin = 3.0F};
    case Genus::Maple:
        return {.coldest = 2.0F, .warmest = 20.0F, .margin = 3.0F};
    case Genus::Palm:
        return {.coldest = 16.0F, .warmest = 40.0F, .margin = 2.0F};
    case Genus::Pine:
        break;
    }
    return {.coldest = -6.0F, .warmest = 21.0F, .margin = 4.0F};
}

// How well a kind grows at `temperature`, 0 to 1, by its range.
[[nodiscard]] float suited(Genus genus, float temperature) noexcept;

// Foliage as a share of the ground's colour under it, plus its own.
struct Foliage {
    float of_ground = 0.0F;
    Vec3 own{0.0F};
};

// Foliage far darker than the ground's mean, so a forest reads as its own mass: needles cool and
// deep, leaves warmer and brighter, a maple's the brightest, a palm's fronds between.
[[nodiscard]] constexpr Foliage foliage_of(Genus genus) noexcept {
    switch (genus) {
    case Genus::Oak:
        return {.of_ground = 0.38F, .own = {0.022F, 0.062F, 0.012F}};
    case Genus::Maple:
        return {.of_ground = 0.38F, .own = {0.03F, 0.072F, 0.01F}};
    case Genus::Palm:
        return {.of_ground = 0.34F, .own = {0.028F, 0.066F, 0.018F}};
    case Genus::Pine:
        break;
    }
    return {.of_ground = 0.3F, .own = {0.012F, 0.05F, 0.028F}};
}

[[nodiscard]] constexpr Vec3 foliage_colour(Genus genus, Vec3 ground) noexcept {
    const Foliage foliage = foliage_of(genus);
    return (ground * foliage.of_ground) + foliage.own;
}

// How tall a genus stands against a pine in the same stand: the broadleaves a little lower, their
// crowns spread rather than raised.
[[nodiscard]] constexpr float stature(Genus genus) noexcept {
    switch (genus) {
    case Genus::Oak:
        return 0.85F;
    case Genus::Maple:
        return 0.9F;
    case Genus::Palm:
    case Genus::Pine:
        break;
    }
    return 1.0F;
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

// The tree the cell grows on `biome` ground at a mean annual `temperature` in °C, if its dice
// land one there.
[[nodiscard]] std::optional<Stood> tree_grown(std::uint32_t lattice_i, std::uint32_t lattice_j,
                                              std::uint32_t face, planet::Biome biome,
                                              float temperature) noexcept;

// The woods about a spot, as tree_grown grows them there averaged over the cells' own dice: for
// drawing woods too far off to stand each tree.
struct Woodland {
    float grown = 0.0F;     // of the lattice cells, the share standing a tree
    float height = 0.0F;    // metres, the trees' mean as pines; the broadleaves stand to stature()
    float broadleaf = 0.0F; // of the trees not palms, the share broadleaves
    float maple = 0.0F;     // of the broadleaves, the share maples
    float palm = 0.0F;      // of the trees, the share palms
};

[[nodiscard]] Woodland woodland_at(Vec2 cell, std::uint32_t face, planet::Biome biome,
                                   float temperature) noexcept;

} // namespace hog::flora
