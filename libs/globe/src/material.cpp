#include <hog/globe/material.hpp>

#include <array>
#include <cstddef>
#include <numbers>

namespace hog::globe {

const Properties& properties(Material material) {
    // Rock melts where its first minerals do, dry; loose ground as the rock it came from. Strength
    // is what a rock mass bears, cracks and all, well under what a sample does. Seepage is
    // hydraulic conductivity: gravelly sand in a day to a metre, loam a metre a day, mud a
    // millimetre; sandstone an aquifer, crystalline rock nearly tight. Loose heaps hold about
    // 35° dry; soil soaked through slumps as mud, sand a little less steep. A flow carries sand
    // at a quarter of a pascal of drag, settled mud and cohesive soil at a few, rock only over
    // ages: its erodibility is stream-power incision, a millimetre a thousand years. Carried, mud
    // settles out at a tenth of a millimetre a second, sand in centimetres, rubble at once.
    constexpr double degree = std::numbers::pi / 180.0;
    static constexpr std::array<Properties, material_count> all{{
        {.density = 1.2, .melts = 0.0, .strength = 0.0, .heat_capacity = 1005.0},       // air
        {.density = 1000.0, .melts = 273.15, .strength = 0.0, .heat_capacity = 4186.0}, // water
        {.density = 300.0,
         .melts = 273.15,
         .strength = 1e4,
         .heat_capacity = 2100.0,
         .porosity = 0.6,
         .conductivity = 1e-3,
         .repose = 38.0 * degree,
         .saturated_repose = 15.0 * degree,
         .critical_shear = 0.1,
         .erodibility = 1e-5,
         .settling = 1e-2}, // snow
        {.density = 1500.0,
         .melts = 1400.0,
         .strength = 1e4,
         .heat_capacity = 800.0,
         .porosity = 0.45,
         .conductivity = 1e-5,
         .repose = 35.0 * degree,
         .saturated_repose = 14.0 * degree,
         .critical_shear = 2.0,
         .erodibility = 1e-6,
         .settling = 1e-3}, // soil
        {.density = 1600.0,
         .melts = 1900.0,
         .strength = 0.0,
         .heat_capacity = 830.0,
         .porosity = 0.4,
         .conductivity = 3e-4,
         .repose = 33.0 * degree,
         .saturated_repose = 25.0 * degree,
         .critical_shear = 0.25,
         .erodibility = 1e-5,
         .settling = 3e-2}, // sand
        {.density = 1800.0,
         .melts = 1400.0,
         .strength = 1e5,
         .heat_capacity = 900.0,
         .porosity = 0.5,
         .conductivity = 1e-7,
         .repose = 30.0 * degree,
         .saturated_repose = 10.0 * degree,
         .critical_shear = 0.5,
         .erodibility = 2e-6,
         .settling = 1e-4}, // sediment
        {.density = 2300.0,
         .melts = 1900.0,
         .strength = 5e7,
         .heat_capacity = 920.0,
         .porosity = 0.15,
         .conductivity = 1e-6,
         .critical_shear = 1e3,
         .erodibility = 3e-13,
         .settling = 3e-1}, // sandstone
        {.density = 2500.0,
         .melts = 1500.0,
         .strength = 3e7,
         .heat_capacity = 900.0,
         .porosity = 0.03,
         .conductivity = 1e-10,
         .critical_shear = 5e2,
         .erodibility = 5e-13,
         .settling = 3e-1}, // shale
        {.density = 2700.0,
         .melts = 1600.0,
         .strength = 6e7,
         .heat_capacity = 910.0,
         .porosity = 0.05,
         .conductivity = 1e-6,
         .critical_shear = 1e3,
         .erodibility = 3e-13,
         .settling = 3e-1}, // limestone
        {.density = 2700.0,
         .melts = 1500.0,
         .strength = 1.3e8,
         .heat_capacity = 790.0,
         .porosity = 0.01,
         .conductivity = 1e-9,
         .critical_shear = 3e3,
         .erodibility = 1e-13,
         .settling = 3e-1}, // granite
        {.density = 3000.0,
         .melts = 1450.0,
         .strength = 1.5e8,
         .heat_capacity = 840.0,
         .porosity = 0.02,
         .conductivity = 1e-9,
         .critical_shear = 3e3,
         .erodibility = 1e-13,
         .settling = 3e-1}, // basalt
        {.density = 3300.0,
         .melts = 1400.0,
         .strength = 1e8,
         .heat_capacity = 1000.0,
         .critical_shear = 3e3,
         .erodibility = 1e-13,
         .settling = 3e-1}, // mantle
        {.density = 7870.0,
         .melts = 1811.0,
         .strength = 2e8,
         .heat_capacity = 450.0,
         .critical_shear = 1e4,
         .erodibility = 1e-14,
         .settling = 5e-1}, // iron
        {.density = 2800.0,
         .melts = 1700.0,
         .strength = 1.3e8,
         .heat_capacity = 800.0,
         .porosity = 0.01,
         .conductivity = 1e-9,
         .critical_shear = 3e3,
         .erodibility = 1e-13,
         .settling = 3e-1}, // anorthosite
        {.density = 1500.0,
         .melts = 1400.0,
         .strength = 1e4,
         .heat_capacity = 750.0,
         .porosity = 0.45,
         .conductivity = 1e-5,
         .repose = 35.0 * degree,
         .saturated_repose = 20.0 * degree,
         .critical_shear = 0.3,
         .erodibility = 1e-5,
         .settling = 3e-2}, // regolith
    }};
    return all[static_cast<std::size_t>(material)];
}

} // namespace hog::globe
