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
    int continents = 6;         // pieces the light crust lies in
    double land_share = 0.3;    // of the surface under light crust
    double plate_speed = 0.45;  // degrees of arc a million years, a typical plate's
    double boundary_age = 40.0; // millions of years a boundary has typically been at work

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

    // Rock as it lies, by what shapes the land: layered rock laid over a hard basement, folded
    // where plates collided, lava over both. What the planet's history laid where is seeded.
    double cover_thickness = 3'000.0;  // metres of layered rock laid on low-standing light crust
    double rift_fill = 4'000.0;        // metres more laid in a rift's sagging valley
    double sea_floor_mud = 5.0;        // metres of mud settling on the sea floor a million years
    double layering = 0.5;             // how strongly hard and soft beds alternate, on average
    double soluble_share = 0.25;       // share of layered rock that dissolves, on average
    double old_folds = 1.0;            // how much of the light crust worn-down old ranges folded
    double arc_lava = 2'000.0;         // metres of lava and ash volcanoes lay behind a trench
    int lava_floods = 6;               // great floods of lava the light crust has known
    double lava_flood_reach = 0.12;    // radians across a typical flood
    double lava_flood_depth = 1'500.0; // metres of lava at a flood's heart
    double exhumed_years = 10.0e6;     // years the land has worn down at today's pace

    // How much richer or poorer than Earth each kind of ore is, against the processes making it.
    struct OreAbundance {
        double copper = 1.0;
        double gold = 1.0;
        double placers = 1.0;
        double banded_iron = 1.0;
        double laterite = 1.0;
        double bog_iron = 1.0;
        double lead_zinc = 1.0;
        double salt = 1.0;
        double coal = 1.0;
        double oil = 1.0;
        double diamonds = 1.0;
    };
    OreAbundance ore_abundance;

    // Sunlight and the air's hold on heat (Budyko's 1969 balance and numbers).
    double star_flux = 1'361.0;   // watts a square metre where the planet orbits
    double tilt = 23.44;          // degrees
    double day = 24.0;            // hours; under 0 spins backwards
    double albedo = 0.32;         // of sunlight thrown back
    double ice_albedo = 0.62;     // where it's cold enough for ice to lie, under -10 °C
    double greenhouse = 202.0;    // W/m² radiated at 0 °C; less holds more heat in
    double heat_transport = 3.04; // W/m² per °C carried polewards
    double lapse_rate = 0.0065;   // °C colder per metre up

    // Water in the air.
    double rain_mean = 1.0;         // metres a year over the whole planet
    double rain_reach = 0.3;        // radians moist air carries inland before most has fallen
    double vapour_height = 2'500.0; // metres of rise that wring out most of the air's water

    // Running water wearing the land (stream power, at Earth's scale).
    double erodibility = 2.0e-6;  // how readily bare rock wears, per year
    double discharge_power = 0.5; // how much more a bigger river cuts
    double cover_holding = 0.5;   // share of wear plants hold back where they cover the ground
    double rock_hardness = 1.0;   // powers of ten slower the strongest rock wears than the softest
    double driest_wear = 0.05;    // metres a year of rain wear acts as if on at least: flash floods
    double wear_threshold = 1.5e-4; // metres a year of cutting a river must pass before it cuts
    double steepest = 0.05;         // steepest mean slope land holds across tens of kilometres

    // Snow lying through summers, gathering into ice that flows and carves.
    double freezing = 0.0;           // °C the liquid freezes at
    double sea_freezing = -1.8;      // °C the open sea freezes at, its salt holding it off
    double season_swing = 20.0;      // °C the year swings either side of its mean near the poles,
                                     // over land at Earth's tilt; none at the equator or no tilt
    double sea_swing = 0.4;          // share of that swing over open liquid, which holds heat
    double melt_rate = 0.005;        // metres of water a day melted per °C over freezing
    double sublimation = 1.0;        // metres of water a year ice gives straight to fully dry air
                                     // at freezing, less the colder as the air holds less
    double gravity = 9.81;           // metres a second squared
    double ice_density = 917.0;      // kilograms a cubic metre
    double ice_strength = 100'000.0; // pascals of shear ice stands on its bed before it flows
    double ice_cooling = 6.0;        // °C colder in the coldest times the planet has known
    double glaciated_years = 100'000.0;  // years the coldest times' ice has had to carve
    double glacial_erosion = 2.7e-7;     // carving per sliding speed to the power below
    double glacial_erosion_power = 2.02; // (Herman et al. 2021, metres and years)
    double overdeepening = 1.5; // steepest a bed rises against the ice, over the ice surface's
                                // slope, before meltwater freezes and carving stops

    // Water standing in hollows.
    double lake_loss = 1.2;       // metres a year open water loses to the air at 15 °C
    double shallowest_lake = 1.0; // metres of hollow under which standing water isn't kept

    // What rivers carry and lay down.
    double carrying = 1.0;           // sediment carried per water times slope: on lowland
                                     // slopes a ten-thousandth of the water, torrents a few %
    double sediment_age = 100'000.0; // years the land has had to gather what its rivers carry

    // Rock turning to soil (Heimsath's soil production).
    double soil_making = 2.0e-4;      // metres a year bare rock weathers at 15 °C, well watered
    double soil_falloff = 2.0;        // per metre: how much slower under deeper soil
    double weathering_warmth = 0.065; // per °C faster as it warms
    double frost_weathering = 0.5; // frost shattering at its peak near -5 °C, against soil_making
    double slowest_wear = 5.0e-6;  // metres a year even the stillest land wears down
    double loose_weathering = 5.0; // how much faster loose till and alluvium turn to soil than rock
    double soil_years = 1'000'000.0;  // years soil has had where the coldest times' ice never lay
    double ice_left_years = 12'000.0; // years since the coldest times' ice melted back
    double rotting_rock = 2.0e-5;     // metres a year the front of rock rotting in place goes down
                                      // at 15 °C, well watered: saprolite under old soils

    // Dead plant matter in and on the soil (Lieth's Miami model of growth; Clymo's peat).
    double plant_growth = 1.0;     // how fast plants grow against Earth's, warmth and rain alike
    double root_share = 0.4;       // share of what plants grow that dies underground
    double humus_share = 0.5;      // share of dead roots that become lasting humus in the soil
    double rotting = 0.03;         // per year humus rots at 10 °C in damp, thawed ground
    double litter_rotting = 10.0;  // how much faster dead matter on top rots than humus, in air
    double drowned_rotting = 0.01; // how much slower anything rots in waterlogged ground
    double peat_burial = 0.1;    // share of what dies on waterlogged ground sinking under the water
                                 // before it rots in the air at the top (Clymo)
    double topsoil_humus = 40.0; // kilograms of humus in a cubic metre of topsoil
    double peat_density =
        100.0; // kilograms of dead plant matter in a cubic metre of litter or peat

    // Wind carrying loose ground off where it lies bare and dry: sand into dunes near by, silt far
    // off as loess.
    double deflation = 0.3;          // share of bare, dry loose ground the wind has taken
    double deflation_depth = 60.0;   // metres down the wind can strip loose ground at most
    double glacial_deflation = 0.15; // share of till and outwash taken while the coldest times'
                                     // ice had just left it bare
    double beach_sand = 0.05;        // share of the sea floor's deposits off a coast blown ashore
    double rock_deflation = 0.03;    // share of what bare, dry rock has weathered the wind takes
    double sand_share = 0.6;         // of river-laid ground, the share that's sand; of what ice
                                     // ground, a third of that, the rest flour
    double silt_share = 0.3;         // of river-laid ground, the share that's silt
    double sand_reach = 150'000.0;   // metres sand goes before most has settled, over bare, dry
                                     // level ground; less where plants or water catch it
    double climb_stop = 0.003;       // rise over run of the ground ahead that stops sand going on
    double silt_reach = 800'000.0;   // metres silt goes before most has settled, in open air
    double silt_trapped = 0.6;       // share less far it goes over ground plants wholly cover
    double thinnest_drift = 0.5;     // metres of wind-laid ground under which plants and burrows
                                     // mix it into the soil rather than it lying as a layer

    // The sea's waves working on its edge, sorting what reaches it into beaches.
    double storm_wind = 20.0; // metres a second the winds of the storms that shape coasts blow
    double swell_share = 0.5; // of a storm's waves, the share reaching coasts off its wind's way
    double shore_years = 7'000.0; // years the sea has stood at about its level, beaches building
    double cliff_retreat = 0.04;  // metres a year waves a metre high wear the softest rock back, as
                                  // their power (their height squared) for bigger ones
    double cliff_hardness = 3.0;  // powers of ten slower they wear the strongest rock back
    double till_stones = 0.3;     // share of what ice laid that's stones
    double rock_sand = 0.35; // share of rock rivers wear off that's sand, the rest silt and clay
    double stone_life = 2'000.0; // years the surf takes to grind most of a beach's stones to sand
    double stirring_waves = 2.0; // metres high storm waves are that wash half the mud off a coast
    double drift_reach = 100'000.0; // metres sand goes along the shore before most has stayed
    double reef_warmth = 23.0;      // °C of yearly sea coral reefs grow best in, few 5 °C colder
    double shelf_depth = 200.0;     // metres of sea about a coast shallow enough for reefs near it
    double reef_sand = 2.0; // cubic metres a year of broken coral a metre of reef-lined coast gets
    double shell_sand = 0.05;   // and of shells, a metre of any open coast
    double muddiest_reef = 1.0; // cubic metres a year of mud passing a metre of coast that smothers
                                // reefs off it
};

} // namespace hog::formation
