#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/climate.hpp>

namespace hog::render {

// Each biome's pines against the stand field: how deep the field must read before woods begin
// (`edge`, 1 grows none), how full the woods are inside, and the odd lone tree in the open.
// Bushes read the same table, crowding the woods' edges and thinning under the canopy.
struct BiomePines {
    float height = 0.0F; // metres; none grows no trees
    float inside = 0.0F; // of lattice cells grown within a stand, 0 to 1
    float edge = 1.0F;   // the stand field's woodland threshold
    float lone = 0.0F;   // of lattice cells grown in the open
};

constexpr BiomePines pines_of(planet::Biome biome) noexcept {
    using enum planet::Biome;
    switch (biome) {
    case BorealForest:
        return {.height = 13.0F, .inside = 0.8F, .edge = 0.42F, .lone = 0.015F};
    case TemperateForest:
        return {.height = 15.0F, .inside = 0.75F, .edge = 0.5F, .lone = 0.012F};
    case Grassland:
        return {.height = 12.0F, .inside = 0.6F, .edge = 0.66F, .lone = 0.0015F};
    case Shrubland:
        return {.height = 7.0F, .inside = 0.5F, .edge = 0.62F, .lone = 0.004F};
    case Alpine:
        return {.height = 4.5F, .inside = 0.5F, .edge = 0.6F, .lone = 0.008F};
    case Wetland:
        return {.height = 10.0F, .inside = 0.55F, .edge = 0.58F, .lone = 0.008F};
    case Tundra:
    case Desert:
    case Savanna:
    case TropicalRainforest: // its broadleaves and palms are species to come
    case Ocean:
    case Beach:
    case Ice:
    case Lake:
        return {};
    }
    return {};
}

// Needles far darker than the ground's mean, cool and green, so a forest reads as its own mass.
constexpr Vec3 needle_colour(Vec3 ground) noexcept {
    return (ground * 0.3F) + (Vec3{0.012F, 0.05F, 0.028F});
}

} // namespace hog::render
