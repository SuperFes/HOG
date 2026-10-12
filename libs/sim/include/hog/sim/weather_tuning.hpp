#pragma once

#include <chrono>

namespace hog::sim {

// The weather aims to look and feel right rather than to be physically exact. Every number
// that shapes it lives here, one group per stage, so stages can be retuned or replaced alone.
using Seconds = std::chrono::duration<double>;

struct HeatTuning {
    // Surface air over land follows the sun within hours; over the ocean it follows the sea.
    Seconds land_response = std::chrono::hours{6};
    Seconds ocean_response = std::chrono::days{20};
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
    double hail_storm = 0.5;   // storm strength where hail starts
    double max_hail = 0.6;     // share of storm rain that falls as hail
    double hail_below = -15.0; // °C in the upper air
};

struct MotionTuning {
    // Surface weather drifts with a wind between the surface's and the jet's, as Earth's fronts
    // and clouds follow the winds a few kilometres up.
    double jet_steering = 0.4;
};

struct WeatherTuning {
    MotionTuning motion{};
    HeatTuning heat{};
    WindTuning wind{};
    WaterTuning water{};
    StormTuning storms{};
};

} // namespace hog::sim
