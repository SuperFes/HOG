#pragma once

#include <hog/mapgen/colour.hpp>
#include <hog/planet/climate.hpp>

namespace hog::mapgen {

// What lies at a point, in the terms a camera sees it by.
struct Surface {
    planet::Biome biome = planet::Biome::Grassland; // Ocean for the sea, Lake for a lake
    double depth = 0.0;                             // metres of sea
    double ice = 0.0;                               // share of the sea iced over, 0 to 1
    double plants = 1.0;                            // share grown of what the biome holds, 0 to 1
    double fire = 0.0;                              // share alight, 0 to 1
    double soil = 0.0;                              // how full the soil is, 0 to 1
    double snow = 0.0;                              // mm of water lying as snow
    double river = 0.0; // share of the point a river covers at its usual flow, 0 to 1
    double flow = 1.0;  // the river's flow over its usual

    friend bool operator==(const Surface&, const Surface&) = default;
};

// The share of the light the surface reflects, as it would look from above in daylight.
[[nodiscard]] LinearRgb true_colour(const Surface& surface) noexcept;

} // namespace hog::mapgen
