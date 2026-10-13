#pragma once

#include "cells.hpp"

#include <hog/sim/weather.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>

// The stages of a weather step. Each updates one cell from the state the stages before it left,
// so cells can run in any order and on any thread.
namespace hog::sim::detail {

inline constexpr double degree = std::numbers::pi / 180.0;
// Distances are taken as if over Earth, so the weather keeps Earth's pace on a small planet.
inline constexpr double earth_radius = 6'371'000.0;
inline constexpr double lapse_rate = 0.0065; // °C per metre

// The sun's declination, as sines: now, and as the slower sea feels it.
struct Seasons {
    double now = 0.0;
    double sea = 0.0;
};

struct StepContext {
    const WeatherConfig* config;
    const CellGrid* grid;
    std::span<const Ground> ground;
    std::span<const float> sea; // °C at the surface
    std::span<const DVec2> slopes;
    std::span<const StormSystem> systems;
    const Calendar* calendar;
    GameTime middle; // halfway through the step
    double seconds;  // the step's length
    Seasons seasons;
    // How freely the land gives up water; without it, as wet as its ground.
    const Land* land = nullptr;
};

// Column water the air holds at saturation, rising about 6% per °C.
[[nodiscard]] inline double saturation(double temperature) {
    return 50.0 * std::exp(0.06 * (temperature - 25.0));
}

// Fraction of the way a value relaxing over `response` moves in `step` seconds.
[[nodiscard]] inline double approach(double step, Seconds response) {
    return 1.0 - std::exp(-step / response.count());
}

// How freely the sea gives up water, as the ice over it allows.
inline constexpr float ice_wetness = 0.2F;

[[nodiscard]] inline double sea_level(double temperature, const Ground& ground) {
    return temperature + (lapse_rate * static_cast<double>(ground.elevation));
}

[[nodiscard]] Seasons seasons(const WeatherConfig& config, GameTime time);

// Where the weather would settle at a latitude and season, before the time of day, over a sea at
// `sea` °C.
struct Targets {
    double surface = 0.0;
    double upper = 0.0;
    // The surface the upper air expects: the cell's mean with the ocean's seasons.
    double reference = 0.0;
};

[[nodiscard]] Targets targets(const WeatherTuning& tuning, const Ground& ground,
                              double sin_latitude, Seasons seasons, double sea);

// Where what is over `frame` now was `seconds` ago, moving at `velocity` (east, north) m/s along a
// great circle.
[[nodiscard]] DVec3 departure(const planet::TangentFrame& frame, Vec2 velocity, double seconds);

// Carries each layer's warmth, water and cloud with the wind, reading the step's start from
// `before`. Moving alone doesn't keep water exactly; conserve_water() puts it right.
void move(const StepContext& step, const WeatherFields& before, WeatherFields& fields,
          std::uint32_t index);
// Scales vapour and cloud so the planet holds as much water as `before` did.
void conserve_water(const CellGrid& grid, const WeatherFields& before, WeatherFields& fields);
// The sun and the seasons pull each layer's temperature towards its targets.
void heat(const StepContext& step, WeatherFields& fields, std::uint32_t index);
// Evaporation, cloud, rain, snow, hail and thunderstorms.
void water(const StepContext& step, WeatherFields& fields, std::uint32_t index);
// Sea-level pressure from the belts and the cell's warmth.
void pressure(const StepContext& step, WeatherFields& fields, std::uint32_t index);
// Surface wind and jet from the pressure and upper air around the cell.
void winds(const StepContext& step, WeatherFields& fields, std::uint32_t index);

// What the recent means start from without a history: wetter ground has wetter weather.
[[nodiscard]] inline float usual_precipitation(const Ground& ground) {
    constexpr float wettest = 4.0F; // mm per day
    return wettest * ground.wetness;
}

// Moves, ages and retires the storm systems, then starts the step's new ones where the jet is
// strongest. `step_number` counts steps since the epoch, so the same step always forms the same
// systems.
void update_systems(const StepContext& step, const WeatherFields& fields,
                    std::vector<StormSystem>& systems, std::uint32_t& next_id,
                    std::int64_t step_number);

// Fronts where the wind brings warmer or colder air.
void fronts(const StepContext& step, WeatherFields& fields, std::uint32_t index);

// How fast the air rises where the winds around the cell converge or climb the ground.
void lift(const StepContext& step, WeatherFields& fields, std::uint32_t index);

} // namespace hog::sim::detail
