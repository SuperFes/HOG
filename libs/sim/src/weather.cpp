#include "weather_stages.hpp"

#include <hog/core/assert.hpp>
#include <hog/planet/noise.hpp>
#include <hog/sim/weather.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace hog::sim {

namespace {

constexpr float cloud_cover_depth = 0.2F; // mm of cloud that covers most of the sky

// Sea ice holds its water in.
float sea_wetness(double temperature) {
    constexpr double freezing = -2.0;
    return temperature < freezing ? 0.2F : 1.0F;
}

GameDuration positive(GameDuration duration) {
    return duration > GameDuration::zero() ? duration : GameDuration{1};
}

} // namespace

WeatherFields::WeatherFields(std::size_t size)
    : temperature(size), upper_temperature(size), pressure(size), vapour(size), cloud(size),
      wind(size, Vec2{0.0F}), jet(size, Vec2{0.0F}), evaporation(size), rain(size), snow(size),
      hail(size), storm(size), lift(size), front(size), recent_temperature(size),
      recent_precipitation(size) {}

float cloud_cover(float cloud) noexcept {
    return 1.0F - std::exp(-cloud / cloud_cover_depth);
}

float WeatherFields::cloud_cover(std::size_t index) const {
    return sim::cloud_cover(cloud[index]);
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
      before_(grid_.size()), time_(detail::last_step(now, positive(config.step))) {
    HOG_ASSERT(config.step > GameDuration::zero(), "weather step must be positive");
    HOG_ASSERT(config.day > GameDuration::zero(), "weather day must be positive");
    HOG_ASSERT(ground_.size() == grid_.size(), "need ground for every cell");
    HOG_ASSERT(config.planet_radius > 0.0, "planet radius must be positive");
    slopes_.reserve(grid_.size());
    for (std::uint32_t index = 0; index < grid_.size(); ++index) {
        DVec2 slope{0.0};
        for (std::size_t k = 0; k < 4; ++k) {
            slope += grid_.gradient_weights(index)[k] *
                     static_cast<double>(ground_[grid_.neighbors(index)[k]].elevation -
                                         ground_[index].elevation);
        }
        slopes_.push_back(slope / config.planet_radius);
    }
    const detail::Seasons seasons = detail::seasons(config_, time_);
    for (std::uint32_t index = 0; index < grid_.size(); ++index) {
        const detail::Targets target =
            detail::targets(config_.tuning.heat, ground_[index], grid_.frame(index).up.z, seasons);
        fields_.temperature[index] = static_cast<float>(target.surface);
        fields_.upper_temperature[index] = static_cast<float>(target.upper);
        fields_.vapour[index] = static_cast<float>(0.7 * detail::saturation(target.surface) *
                                                   static_cast<double>(ground_[index].wetness));
        fields_.recent_temperature[index] = static_cast<float>(target.surface);
        fields_.recent_precipitation[index] = detail::usual_precipitation(ground_[index]);
    }
    balance_winds(nullptr);
}

void Weather::set_fields(WeatherFields fields) {
    HOG_ASSERT(fields.size() == grid_.size() && fields.wind.size() == grid_.size(),
               "need weather for every cell");
    fields_ = std::move(fields);
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
        .slopes = slopes_,
        .systems = {},
        .calendar = &calendar_,
        .middle = middle,
        .seconds = Seconds{config_.step}.count(),
        .seasons = detail::seasons(config_, middle),
    };
    detail::update_systems(context, fields_, systems_, next_system_,
                           at.time_since_epoch() / config_.step);
    before_ = fields_;
    detail::for_cells(&jobs, grid_.size(),
                      [&](std::uint32_t index) { detail::move(context, before_, fields_, index); });
    detail::conserve_water(grid_, before_, fields_);
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
        .slopes = slopes_,
        .systems = systems_,
        .calendar = &calendar_,
        .middle = time_,
        .seconds = 0.0,
        .seasons = detail::seasons(config_, time_),
    };
    detail::for_cells(jobs, grid_.size(),
                      [&](std::uint32_t index) { detail::pressure(context, fields_, index); });
    detail::for_cells(jobs, grid_.size(),
                      [&](std::uint32_t index) { detail::winds(context, fields_, index); });
    detail::for_cells(jobs, grid_.size(),
                      [&](std::uint32_t index) { detail::fronts(context, fields_, index); });
    detail::for_cells(jobs, grid_.size(),
                      [&](std::uint32_t index) { detail::lift(context, fields_, index); });
}

} // namespace hog::sim
