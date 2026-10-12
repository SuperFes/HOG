#include "weather_stages.hpp"

#include <hog/core/assert.hpp>
#include <hog/planet/noise.hpp>
#include <hog/sim/weather.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace hog::sim {

namespace {

constexpr float cloud_cover_depth = 0.5F; // mm of cloud that covers most of the sky

// Sea ice holds its water in.
float sea_wetness(double temperature) {
    constexpr double freezing = -2.0;
    return temperature < freezing ? 0.2F : 1.0F;
}

GameTime last_step(GameTime now, GameDuration step) {
    const auto since = now.time_since_epoch();
    auto steps = since / step;
    if (since % step < GameDuration::zero()) {
        --steps;
    }
    return GameTime{steps * step};
}

GameDuration positive(GameDuration duration) {
    return duration > GameDuration::zero() ? duration : GameDuration{1};
}

} // namespace

WeatherFields::WeatherFields(std::size_t size)
    : temperature(size), upper_temperature(size), pressure(size), vapour(size), cloud(size),
      wind(size, Vec2{0.0F}), jet(size, Vec2{0.0F}), evaporation(size), rain(size), snow(size),
      hail(size), storm(size) {}

float WeatherFields::cloud_cover(std::size_t index) const {
    return 1.0F - std::exp(-cloud[index] / cloud_cover_depth);
}

std::vector<Ground> ground_for(const CellGrid& grid, const planet::World& world) {
    const planet::Climate& climate = world.geology().climate();
    std::vector<Ground> ground(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const planet::CellId cell = grid.cell(index);
        const planet::CellState state = world.state(cell);
        const bool ocean = state.biome == planet::Biome::Ocean;
        const double temperature = planet::fixed_to_double(climate.temperature(cell));
        ground[index] = Ground{
            .mean_temperature = static_cast<float>(temperature),
            .elevation =
                ocean ? 0.0F
                      : static_cast<float>(std::max(planet::fixed_to_double(state.elevation), 0.0)),
            .wetness = ocean ? sea_wetness(temperature)
                             : static_cast<float>(planet::fixed_to_double(climate.moisture(cell))),
            .ocean = ocean,
        };
    }
    return ground;
}

Weather::Weather(const WeatherConfig& config, std::vector<Ground> ground, GameTime now)
    : config_(config), grid_(config.level), ground_(std::move(ground)),
      calendar_(positive(config.day), config.axial_tilt), fields_(grid_.size()),
      time_(last_step(now, positive(config.step))) {
    HOG_ASSERT(config.step > GameDuration::zero(), "weather step must be positive");
    HOG_ASSERT(config.day > GameDuration::zero(), "weather day must be positive");
    HOG_ASSERT(ground_.size() == grid_.size(), "need ground for every cell");
    const double sin_declination = detail::sin_declination(time_, config_.axial_tilt);
    for (std::uint32_t index = 0; index < grid_.size(); ++index) {
        const detail::Targets target = detail::targets(config_.tuning.heat, ground_[index],
                                                       grid_.frame(index).up.z, sin_declination);
        fields_.temperature[index] = static_cast<float>(target.surface);
        fields_.upper_temperature[index] = static_cast<float>(target.upper);
        fields_.vapour[index] = static_cast<float>(0.7 * detail::saturation(target.surface) *
                                                   static_cast<double>(ground_[index].wetness));
    }
    balance_winds(nullptr);
}

std::uint64_t Weather::advance_to(GameTime now, jobs::JobSystem& jobs) {
    std::uint64_t steps = 0;
    while (time_ + config_.step <= now) {
        step(time_ + config_.step, jobs);
        ++steps;
    }
    return steps;
}

void Weather::step(GameTime at, jobs::JobSystem& jobs) {
    const GameTime middle = at - (config_.step / 2);
    const detail::StepContext context{
        .config = &config_,
        .grid = &grid_,
        .ground = ground_,
        .calendar = &calendar_,
        .middle = middle,
        .seconds = Seconds{config_.step}.count(),
        .sin_declination = detail::sin_declination(middle, config_.axial_tilt),
    };
    detail::for_cells(&jobs, grid_.size(), [&](std::uint32_t index) {
        detail::heat(context, fields_, index);
        detail::water(context, fields_, index);
    });
    time_ = at;
    balance_winds(&jobs);
}

void Weather::balance_winds(jobs::JobSystem* jobs) {
    const detail::StepContext context{
        .config = &config_,
        .grid = &grid_,
        .ground = ground_,
        .calendar = &calendar_,
        .middle = time_,
        .seconds = 0.0,
        .sin_declination = detail::sin_declination(time_, config_.axial_tilt),
    };
    detail::for_cells(jobs, grid_.size(),
                      [&](std::uint32_t index) { detail::pressure(context, fields_, index); });
    detail::for_cells(jobs, grid_.size(),
                      [&](std::uint32_t index) { detail::winds(context, fields_, index); });
}

} // namespace hog::sim
