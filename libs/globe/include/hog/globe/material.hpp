#pragma once

#include <cstdint>

namespace hog::globe {

// What a hexel is made of. Whether it's solid, molten or a gas comes from its temperature, so a
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
};

inline constexpr int material_count = 14;

// Only what changes how the world forms, at the surface's pressure.
struct Properties {
    double density = 0.0;  // kg/m³
    double melts = 0.0;    // kelvin; none for air
    double strength = 0.0; // pascals it bears cold before it gives way; loose ground bears little
    double heat_capacity = 0.0; // J/kg/K
};

[[nodiscard]] const Properties& properties(Material material);

} // namespace hog::globe
