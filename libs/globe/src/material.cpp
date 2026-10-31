#include <hog/globe/material.hpp>

#include <array>
#include <cstddef>

namespace hog::globe {

const Properties& properties(Material material) {
    // Rock melts where its first minerals do, dry; loose ground as the rock it came from.
    static constexpr std::array<Properties, material_count> all{{
        {.density = 1.2, .melts = 0.0},       // air
        {.density = 1000.0, .melts = 273.15}, // water
        {.density = 300.0, .melts = 273.15},  // snow
        {.density = 1500.0, .melts = 1400.0}, // soil
        {.density = 1600.0, .melts = 1900.0}, // sand
        {.density = 1800.0, .melts = 1400.0}, // sediment
        {.density = 2300.0, .melts = 1900.0}, // sandstone
        {.density = 2500.0, .melts = 1500.0}, // shale
        {.density = 2700.0, .melts = 1600.0}, // limestone
        {.density = 2700.0, .melts = 1500.0}, // granite
        {.density = 3000.0, .melts = 1450.0}, // basalt
        {.density = 3300.0, .melts = 1400.0}, // mantle
        {.density = 7870.0, .melts = 1811.0}, // iron
    }};
    return all[static_cast<std::size_t>(material)];
}

} // namespace hog::globe
