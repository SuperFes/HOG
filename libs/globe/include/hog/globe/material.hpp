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
    double settling = 0.0;         // m/s it sinks through still water once carried
    // What holds a thing on it. A loose material sticks together by `cohesion` (pascals) and
    // slides on a hard bed at `bed_friction`; hard ground grips what lies on it or walks on it at
    // `friction`, dry, and `wet_friction` when a film of water or melt is on it. Radians from level.
    double cohesion = 0.0;
    double bed_friction = 0.0;
    double friction = 0.0;
    double wet_friction = 0.0;
    // Share of its speed against a surface a falling lump of it keeps on rebounding; rubble of
    // rock by default, loose ground dead.
    double bounce = 0.3;
};

[[nodiscard]] const Properties& properties(Material material);

// The steepest slope, in radians from level, something stands on bare `material` at, `wetness`
// from 0 dry to 1 soaked: a heap's repose if loose, else the rock's grip.
[[nodiscard]] double footing(Material material, double wetness);

// The tangent of the steepest slope a layer of `grain`, `depth` metres deep, holds on a bed of
// `bed` under `gravity`, `wetness` from 0 dry to 1 soaked. A thick layer is held by its own friction; a thin one by its grip on the
// bed, which loose ground gives as much as it holds itself and hard ground as its surface does;
// and cohesion holds a dusting of snow or clay where dry sand would run.
[[nodiscard]] double holding_slope(Material grain, Material bed, double wetness, double depth,
                                   double gravity);

} // namespace hog::globe
