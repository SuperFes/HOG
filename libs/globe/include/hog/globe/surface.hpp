#pragma once

#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace hog::globe {

// What a place on the ground is: the shaped matter at the top of a column, the loose grain lying
// on it, and how wet. The one description everything that acts on the ground reads: grain
// slumping, footing underfoot, and whatever comes to roll or leave a mark on it.
struct Surface {
    std::uint32_t first = 0;        // the layer of the lowest open voxel over the bed
    Material bed = Material::Air;   // the shaped matter under it
    Voxel bottom{};                 // that open voxel, which holds the shaped surface's distance
    Material grain = Material::Air; // what lies on the bed, the first kind found
    std::int64_t grain_amount = 0;  // all of it, in units where Voxel::full is a voxel's height
    std::uint32_t open = 0;         // open voxels up from `first`, where grain has room
    double wetness = 0.0;           // from 0 dry to 1 soaked, by the water in bed and grain

    // The tangent of the steepest slope this surface holds: its grain on its bed, or the bed bare.
    [[nodiscard]] double holds(double thickness, double gravity) const;
};

// Down a column from layer `start` to the first shaped matter with open voxel over it, going at
// most `reach` layers, then up through the open voxels over it, at most `tallest`. `lookup(layer)`
// gives the voxel there, or null where it isn't known, which ends the search.
template <typename Lookup>
[[nodiscard]] std::optional<Surface> surface_below(std::uint32_t start, std::uint32_t reach,
                                                   std::uint32_t tallest, double thickness,
                                                   const Lookup& lookup) {
    Surface surface;
    double wet = 0.0;
    std::uint32_t layer = start;
    for (std::uint32_t steps = 0;; ++steps) {
        const Voxel* here = lookup(layer);
        if (here == nullptr) {
            return std::nullopt;
        }
        if (here->solid != Material::Air) {
            surface.bed = here->solid;
            wet = here->liquid == Material::Water
                      ? dampness(here->solid, here->liquid_amount, thickness)
                      : 0.0;
            break;
        }
        if (layer == 0 || steps >= reach) {
            return std::nullopt;
        }
        --layer;
    }
    surface.first = layer + 1;
    const Voxel* bottom = lookup(surface.first);
    if (bottom == nullptr || bottom->solid != Material::Air) {
        return std::nullopt;
    }
    surface.bottom = *bottom;
    if (bottom->liquid == Material::Water) {
        wet = std::max(wet, dampness(Material::Air, bottom->liquid_amount, thickness));
    }
    for (std::uint32_t up = 0; up < tallest; ++up) {
        const Voxel* here = lookup(surface.first + up);
        if (here == nullptr || here->solid != Material::Air) {
            break;
        }
        surface.open = up + 1;
        surface.grain_amount += here->grain_amount;
        if (surface.grain == Material::Air && here->grain_amount > 0) {
            surface.grain = here->grain;
        }
    }
    surface.wetness = std::min(wet, 1.0);
    return surface;
}

inline constexpr std::uint32_t any_depth = std::numeric_limits<std::uint32_t>::max();

} // namespace hog::globe
