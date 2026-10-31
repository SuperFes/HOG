#include <hog/globe/material.hpp>

#include <array>
#include <cstddef>

namespace hog::globe {

const Properties& properties(Material material) {
    // Rock melts where its first minerals do, dry; loose ground as the rock it came from. Strength
    // is what a rock mass bears, cracks and all, well under what a sample does.
    static constexpr std::array<Properties, material_count> all{{
        {.density = 1.2, .melts = 0.0, .strength = 0.0, .heat_capacity = 1005.0},       // air
        {.density = 1000.0, .melts = 273.15, .strength = 0.0, .heat_capacity = 4186.0}, // water
        {.density = 300.0, .melts = 273.15, .strength = 1e4, .heat_capacity = 2100.0},  // snow
        {.density = 1500.0, .melts = 1400.0, .strength = 1e4, .heat_capacity = 800.0},  // soil
        {.density = 1600.0, .melts = 1900.0, .strength = 0.0, .heat_capacity = 830.0},  // sand
        {.density = 1800.0, .melts = 1400.0, .strength = 1e5, .heat_capacity = 900.0},  // sediment
        {.density = 2300.0, .melts = 1900.0, .strength = 5e7, .heat_capacity = 920.0},  // sandstone
        {.density = 2500.0, .melts = 1500.0, .strength = 3e7, .heat_capacity = 900.0},  // shale
        {.density = 2700.0, .melts = 1600.0, .strength = 6e7, .heat_capacity = 910.0},  // limestone
        {.density = 2700.0, .melts = 1500.0, .strength = 1.3e8, .heat_capacity = 790.0}, // granite
        {.density = 3000.0, .melts = 1450.0, .strength = 1.5e8, .heat_capacity = 840.0}, // basalt
        {.density = 3300.0, .melts = 1400.0, .strength = 1e8, .heat_capacity = 1000.0},  // mantle
        {.density = 7870.0, .melts = 1811.0, .strength = 2e8, .heat_capacity = 450.0},   // iron
    }};
    return all[static_cast<std::size_t>(material)];
}

} // namespace hog::globe
