#pragma once

#include <hog/globe/cell.hpp>

namespace hog::globe {

// How the voxels stand outward: even layers from an impassable floor up to the sky's ceiling.
// Below the floor and above the ceiling there are no voxels, only what's calculated.
struct Layers {
    double floor = 0.0;     // metres from the planet's centre
    double thickness = 1.0; // metres
    int count = 0;

    [[nodiscard]] constexpr double bottom(int layer) const noexcept {
        return floor + (layer * thickness);
    }
    [[nodiscard]] constexpr double top(int layer) const noexcept { return bottom(layer + 1); }
    [[nodiscard]] constexpr double ceiling() const noexcept { return bottom(count); }

    // The layer holding `radius`: -1 below the floor, `count` at or above the ceiling.
    [[nodiscard]] int layer_at(double radius) const noexcept;

    // Cubic metres in the voxel of `cell` in `layer`.
    [[nodiscard]] double volume(Cell cell, int layer) const;
};

// Layers from `depth` below `radius` to `height` above it, as near `thickness` thick as fit.
[[nodiscard]] Layers layers_about(double radius, double depth, double height, double thickness);

} // namespace hog::globe
