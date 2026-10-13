#pragma once

#include <chrono>
#include <cstddef>

namespace hog::sim {

// The weather aims to look and feel right rather than to be physically exact. Every number
// that shapes it lives here, one group per stage, so stages can be retuned or replaced alone.
using Seconds = std::chrono::duration<double>;

struct HeatTuning {
    // Surface air over land follows the sun within hours; over the ocean it follows the sea.
    Seconds land_response = std::chrono::hours{6};
    Seconds ocean_response = std::chrono::days{2};
    // The open sea's seasons as the air's pressure is measured against: a month late and milder.
    Seconds ocean_lag = std::chrono::days{35};
    Seconds upper_response = std::chrono::days{4};
    double land_seasons = 55.0; // °C per unit of sin(declination) · sin(latitude)
    double ocean_seasons = 15.0;
    double land_days = 12.0; // °C per unit of sin(sun elevation)
    double ocean_days = 0.6;
    double cloud_shade = 0.6; // share of the day's heating a full overcast blocks

    // The upper air cools across a band whose middle follows the sun; the jet runs along it.
    // Angles are in degrees.
    double upper_equator = -6.0; // °C
    double upper_drop = 32.0;    // °C from the equator to the poles
    double upper_drop_seasons = 15.0;
    double band_latitude = 45.0;
    double band_seasons = 15.0;
    double band_width = 15.0;
};

struct WindTuning {
    // Belts of high and low pressure like Earth's: low at the equator, high near 30°, low
    // near 60°.
    double band_pressure = 8.0;    // hPa
    double thermal_pressure = 0.5; // hPa per °C of surface warmth over the upper air
    double ocean_friction = 4e-5;  // 1/s
    double land_friction = 8e-5;
    double max_wind = 50.0; // m/s
    double max_jet = 90.0;
    // The Coriolis force is held away from zero, where the thermal wind is undefined.
    double min_coriolis_latitude = 10.0; // degrees
};

struct WaterTuning {
    double evaporation_per_day = 0.25; // of the shortfall from saturation
    double condensing_humidity = 0.9;
    Seconds cloud_clearing = std::chrono::hours{3};
    double raining_cloud = 0.4; // mm of cloud before it rains
    Seconds rain_out = std::chrono::hours{2};
    double snow_below = 1.5; // °C at the surface; all snow 3 °C lower
};

// Thunderstorms grow where warm, moist surface air sits under cold upper air. The surface's
// moisture counts as warmth it releases on rising: a millimetre of column water is roughly a
// degree.
struct StormTuning {
    double neutral_contrast = 75.0; // °C
    double full_contrast = 10.0;    // °C beyond neutral for a full storm
    Seconds response = std::chrono::hours{1};
    double rain_out = 0.3; // share of the column's vapour a full storm rains out per response
    double anvil = 0.2;    // share of that left as cloud
    double mixing = 0.3;   // share of the contrast stirred away per response
    // Most of a storm's heat is released aloft; the sea holds the air above it where it is.
    double land_cooling = 0.2;
    double hail_storm = 0.4;   // storm strength where hail starts
    double max_hail = 0.6;     // share of storm rain that falls as hail
    double hail_below = -15.0; // °C in the upper air
};

struct MotionTuning {
    // Surface weather drifts with a wind between the surface's and the jet's, as Earth's fronts
    // and clouds follow the winds a few kilometres up.
    double jet_steering = 0.4;
};

// Rising air cools, so it clouds over and rains sooner; sinking air dries out. Air rises where
// surface winds converge and where they blow up a slope.
struct LiftTuning {
    double converging_depth = 1'000.0; // metres of air squeezed upwards where winds converge
    // Share of the ground's real steepness the wind climbs; the planet's slopes are steep for
    // the distances weather travels.
    double slope_share = 0.05;
    double reference = 0.05; // m/s of rising that counts as strong
    double humidity = 0.25;  // how much sooner strongly rising air condenses
    double storm = 6.0;      // °C of storm contrast strongly rising air adds
    // Sinking air warms as it is squeezed, so it holds more water and its cloud melts away.
    double foehn = 8.0; // °C strongly sinking air warms by
};

// Travelling lows, born where the jet is strongest and carried along by it. Their winds pull warm
// air poleward ahead of them and cold air in behind, which makes the fronts.
struct SystemTuning {
    double per_day = 2.5; // new systems in each hemisphere
    double min_latitude = 25.0;
    double max_latitude = 65.0;
    double min_depth = 10.0; // hPa below the surroundings at their deepest
    double max_depth = 30.0;
    double min_radius = 8.0; // degrees
    double max_radius = 14.0;
    Seconds min_life = std::chrono::days{3};
    Seconds max_life = std::chrono::days{7};
    double growing = 0.35;  // share of a life spent deepening
    double steering = 0.6;  // share of the jet's speed they travel at
    double poleward = 1.0;  // m/s they drift towards the pole
    std::size_t limit = 40; // systems alive at once
    double spacing = 2.0;   // radii a new system keeps from the others
};

// Each low carries fronts the way Earth's do: a warm front reaching ahead of it and a cold front
// trailing behind towards the equator. Air rises along both, gently at the warm front and sharply
// at the cold. Lengths are in the system's radii, east and poleward of its centre.
struct FrontTuning {
    double warm_east = 1.4;
    double warm_poleward = -0.4;
    double cold_east = -0.4;
    double cold_poleward = -1.4;
    double width = 0.3;
    double lift = 0.06;      // m/s of rising along a strong front
    double cold_storm = 8.0; // °C of storm contrast a strong cold front adds
};

// Running means of the weather, for what lives under it and responds over days, not hours.
struct RecentTuning {
    Seconds temperature = std::chrono::days{5};
    Seconds precipitation = std::chrono::days{20};
};

// Surface currents from the wind-driven gyre model: the twist of the wind over the sea, the
// planet's spin growing towards the poles and friction balance, which turns each basin's water in
// gyres and crowds their return flow against the western coasts. Distances are Earth's, as the
// air's.
struct OceanTuning {
    Seconds step = std::chrono::hours{6};
    Seconds stress_memory = std::chrono::days{10}; // storms pass without stirring the gyres
    double drag = 1.3e-3;                          // of the wind on the sea
    double layer = 50.0;                           // metres of water the wind drives
    double friction = 5e-6;     // 1/s; the western currents are about friction / spin gradient wide
    double max_current = 2.0;   // m/s
    int settling = 20;          // solver sweeps a step
    int first_settling = 5'000; // solver sweeps at most, from rest
    // A new sea runs this long before it is first used, in day-long steps, to settle its warmth.
    Seconds spin_up = std::chrono::days{180};

    // The sea's warmth follows the season this slowly, so it lags the sun by over a month.
    Seconds response = std::chrono::days{40};
    double seasons = 18.0;  // °C per unit of sin(declination) · sin(latitude)
    double upwelled = 8.0;  // °C cooler than the season, the water that wells up from below
    double freezing = -2.0; // °C, below which the sea ices over instead
    // The wind's sideways push on the water is held finite near the equator, where it would be
    // infinite. Degrees.
    double min_spin_latitude = 10.0;
};

struct WeatherTuning {
    MotionTuning motion{};
    LiftTuning lift{};
    SystemTuning systems{};
    FrontTuning fronts{};
    HeatTuning heat{};
    WindTuning wind{};
    WaterTuning water{};
    StormTuning storms{};
    RecentTuning recent{};
    OceanTuning ocean{};
};

} // namespace hog::sim
