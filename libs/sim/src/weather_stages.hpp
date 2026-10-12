#pragma once

#include <hog/jobs/job_system.hpp>
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

struct StepContext {
    const WeatherConfig* config;
    const CellGrid* grid;
    std::span<const Ground> ground;
    std::span<const DVec2> slopes;
    const Calendar* calendar;
    GameTime middle; // halfway through the step
    double seconds;  // the step's length
    double sin_declination;
};

// Column water the air holds at saturation, rising about 6% per °C.
[[nodiscard]] inline double saturation(double temperature) {
    return 50.0 * std::exp(0.06 * (temperature - 25.0));
}

// Fraction of the way a value relaxing over `response` moves in `step` seconds.
[[nodiscard]] inline double approach(double step, Seconds response) {
    return 1.0 - std::exp(-step / response.count());
}

[[nodiscard]] inline double sea_level(double temperature, const Ground& ground) {
    return temperature + (lapse_rate * static_cast<double>(ground.elevation));
}

[[nodiscard]] double sin_declination(GameTime time, double axial_tilt);

// Where the weather would settle at a latitude and season, before the time of day.
struct Targets {
    double surface = 0.0;
    double upper = 0.0;
    // The surface the upper air expects: the cell's mean with the ocean's seasons.
    double reference = 0.0;
};

[[nodiscard]] Targets targets(const HeatTuning& tuning, const Ground& ground, double sin_latitude,
                              double sin_declination);

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

// How fast the air rises where the winds around the cell converge or climb the ground.
void lift(const StepContext& step, WeatherFields& fields, std::uint32_t index);

// Runs body(index) for every cell, across the jobs when given.
template <typename F>
void for_cells(jobs::JobSystem* jobs, std::uint32_t count, F body) {
    constexpr std::uint32_t cells_per_job = 256;
    if (jobs == nullptr) {
        for (std::uint32_t index = 0; index < count; ++index) {
            body(index);
        }
        return;
    }
    jobs->parallel_for(count, cells_per_job, [&](jobs::Range range, std::uint32_t /*thread*/) {
        for (std::uint32_t index = range.begin; index < range.end; ++index) {
            body(index);
        }
    });
}

} // namespace hog::sim::detail
