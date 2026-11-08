#pragma once

#include <cstdint>

namespace hog::formation {

// The planet's knobs. Distances along the surface are radians of arc, so a feature keeps its
// share of the planet at any radius; heights are metres. Defaults make an Earth-like world, which
// is only a starting point.
struct Body {
    std::uint64_t seed = 1;
    double radius = 115'000.0; // metres

    // Plates, and the thick light crust riding on them.
    int plates = 14;
    int continents = 6;           // pieces the light crust lies in
    double land_share = 0.3;      // of the surface under light crust
    double plate_speed = 0.45;    // degrees of arc a million years, a typical plate's
    double boundary_age = 40.0;   // millions of years a boundary has typically been at work

    // How crust stands: light crust floats high, sea floor sinks as it ages and cools.
    double crust_thickness = 35'000.0; // metres of light crust
    double crust_rise = 0.15;          // metres light crust rises per metre it thickens
    double continent_height = 400.0;   // metres over the liquid, unthickened
    double ridge_depth = -2'600.0;     // metres, where new sea floor is made
    double abyss_depth = -5'500.0;     // metres, the deepest old sea floor
    double floor_sinking = 350.0;      // metres per square root of a million years of age
    double smallest_sea = 0.002;       // share of the surface water under the level must join up
                                       // over to be open sea; smaller is a basin on land

    // What happens where plates meet.
    double belt_width = 0.035;     // radians from a boundary to a belt's edge
    double thickening = 0.25;      // share of a collision's shortening kept as thicker crust
    double trench_depth = 3'000.0; // metres under the sea floor beside it
    double arc_height = 2'000.0;   // metres volcanoes build behind a trench
    double rift_depth = 1'500.0;   // metres a continent sags where it's pulled apart
    double uplift_share = 0.15;    // of the closing speed that pushes rock up in a belt

    // Sunlight and the air's hold on heat (Budyko's 1969 balance and numbers).
    double star_flux = 1'361.0;     // watts a square metre where the planet orbits
    double tilt = 23.44;            // degrees
    double day = 24.0;              // hours; under 0 spins backwards
    double albedo = 0.32;           // of sunlight thrown back
    double ice_albedo = 0.62;       // where it's cold enough for ice to lie, under -10 °C
    double greenhouse = 202.0;      // W/m² radiated at 0 °C; less holds more heat in
    double heat_transport = 3.04;   // W/m² per °C carried polewards
    double lapse_rate = 0.0065;     // °C colder per metre up

    // Water in the air.
    double rain_mean = 1.0;         // metres a year over the whole planet
    double rain_reach = 0.3;        // radians moist air carries inland before most has fallen
    double vapour_height = 2'500.0; // metres of rise that wring out most of the air's water

    // Running water wearing the land (stream power, at Earth's scale).
    double erodibility = 2.0e-6;   // how readily bare rock wears, per year
    double discharge_power = 0.5;  // how much more a bigger river cuts
    double cover_holding = 0.5;    // share of wear plants hold back where they cover the ground
    double driest_wear = 0.05;     // metres a year of rain wear acts as if on at least: flash floods
    double wear_threshold = 1.5e-4; // metres a year of cutting a river must pass before it cuts
    double steepest = 0.05;        // steepest mean slope land holds across tens of kilometres

    // Water standing in hollows.
    double lake_loss = 1.2;        // metres a year open water loses to the air at 15 °C
    double shallowest_lake = 1.0;  // metres of hollow under which standing water isn't kept

    // Rock turning to soil (Heimsath's soil production).
    double soil_making = 2.0e-4;   // metres a year bare rock weathers at 15 °C, well watered
    double soil_falloff = 2.0;     // per metre: how much slower under deeper soil
    double weathering_warmth = 0.065; // per °C faster as it warms
    double frost_weathering = 0.5; // frost shattering at its peak near -5 °C, against soil_making
    double slowest_wear = 5.0e-6;  // metres a year even the stillest land wears down
};

} // namespace hog::formation
