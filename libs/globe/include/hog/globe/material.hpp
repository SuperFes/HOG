#pragma once

#include <cstdint>

namespace hog::globe {

// What matter is made of. Whether it's solid, molten or a gas comes from its temperature, so a
// melted mantle is still mantle and steam is still water.
enum class Material : std::uint8_t {
    Air,
    Water,
    Snow,
    Soil,
    Sand,
    Sediment,
    Sandstone,
    Shale,
    Limestone,
    Granite,
    Basalt,
    Mantle, // peridotite
    Iron,
    Anorthosite, // the Moon's pale highland rock
    Regolith,    // rock shattered and gardened by impacts, where no air weathers it
};

inline constexpr int material_count = 15;

// Only what changes how the world forms, at the surface's pressure.
struct Properties {
    double density = 0.0;  // kg/m³
    double melts = 0.0;    // kelvin; none for air
    double strength = 0.0; // pascals it bears cold before it gives way; loose ground bears little
    double heat_capacity = 0.0; // J/kg/K
    // How liquid gets into it and moves it.
    double porosity = 0.0;         // share of its volume that is pore space a liquid can fill
    double conductivity = 0.0;     // m/s a liquid seeps through it under its own head
    double repose = 0.0;           // radians from level a loose heap of it holds, dry
    double saturated_repose = 0.0; // and soaked through, when soil slumps as mud
    double critical_shear = 0.0;   // pascals of a flow's drag it withstands before it's carried
    double erodibility = 0.0;      // m/s of it carried off per pascal over that
};

[[nodiscard]] const Properties& properties(Material material);

} // namespace hog::globe
