#include "weather_stages.hpp"

#include <hog/core/assert.hpp>
#include <hog/planet/noise.hpp>
#include <hog/sim/weather.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

namespace hog::sim {

namespace {

constexpr float cloud_cover_depth = 0.2F; // mm of cloud that covers most of the sky

// Sea ice holds its water in.
float sea_wetness(double temperature) {
    return temperature <= OceanTuning{}.freezing ? detail::ice_wetness : 1.0F;
}

GameDuration positive(GameDuration duration) {
    return duration > GameDuration::zero() ? duration : GameDuration{1};
}

double rotation(const WeatherConfig& config) {
    return 2.0 * std::numbers::pi / Seconds{positive(config.day)}.count();
}

GameDuration ocean_step(const WeatherConfig& config) {
    return positive(std::chrono::round<GameDuration>(config.tuning.ocean.step));
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

Sky sky_over(const WeatherFields& fields, const Ground& ground, const WeatherTuning& tuning,
             std::size_t index) {
    const SkyTuning& sky = tuning.sky;
    const auto surface = static_cast<double>(fields.temperature[index]);
    const double holds = tuning.water.condensing_humidity * detail::saturation(surface);
    const double humidity = std::clamp(static_cast<double>(fields.vapour[index]) / holds, 0.0, 1.0);
    const double rising =
        std::max(std::tanh(static_cast<double>(fields.lift[index]) / tuning.lift.reference), 0.0);
    const auto storm = static_cast<double>(fields.storm[index]);
    const auto elevation = static_cast<double>(ground.elevation);

    const auto smooth = [](double from, double to, double at) {
        const double t = std::clamp((at - from) / (to - from), 0.0, 1.0);
        return t * t * (3.0 - (2.0 * t));
    };
    const Vec2 wind = fields.wind[index];
    const double speed = std::hypot(static_cast<double>(wind.x), static_cast<double>(wind.y));
    const double fog = smooth(sky.fog_humidity, 1.0, humidity) *
                       (1.0 - smooth(sky.fog_calm, sky.fog_windy, speed)) * (1.0 - rising) *
                       (1.0 - storm);

    const double fog_top = fog * (std::max(elevation, 0.0) + sky.fog_depth);
    const double dew_base =
        elevation + std::min(sky.base_per_degree *
                                 detail::dew_spread(humidity * tuning.water.condensing_humidity),
                             sky.highest_base);
    // Fog is cloud that's come down as the air cooled.
    const double base = std::lerp(dew_base, fog_top, fog);
    const double depth = sky.thinnest +
                         (sky.depth_per_mm * static_cast<double>(fields.cloud[index])) +
                         (sky.rising_depth * rising);
    const double top = std::max(base + depth, sky.storm_top * storm);
    return {.humidity = static_cast<float>(humidity),
            .fog = static_cast<float>(fog),
            .fog_top = static_cast<float>(fog_top),
            .base = static_cast<float>(base),
            .top = static_cast<float>(top)};
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
      before_(grid_.size()), time_(detail::last_step(now, positive(config.step))),
      ocean_(grid_, ground_, config.tuning.ocean, rotation(config),
             detail::seasons(config, time_).now),
      land_(grid_, ground_, config.tuning.land, config.planet_radius) {
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
    start_air();
    spin_up_ocean();
    start_air();
    spin_up_land();
}

void Weather::start_air() {
    const detail::Seasons seasons = detail::seasons(config_, time_);
    for (std::uint32_t index = 0; index < grid_.size(); ++index) {
        const detail::Targets target =
            detail::targets(config_.tuning, ground_[index], grid_.frame(index).up.z, seasons,
                            static_cast<double>(ocean_.fields().temperature[index]));
        fields_.temperature[index] = static_cast<float>(target.surface);
        fields_.upper_temperature[index] = static_cast<float>(target.upper);
        fields_.vapour[index] = static_cast<float>(0.7 * detail::saturation(target.surface) *
                                                   static_cast<double>(ground_[index].wetness));
        fields_.recent_temperature[index] = static_cast<float>(target.surface);
        fields_.recent_precipitation[index] = detail::usual_precipitation(ground_[index]);
    }
    balance_winds(nullptr);
}

void Weather::spin_up_ocean() {
    ocean_.settle_under(grid_, fields_.wind, nullptr);
    constexpr GameDuration day = std::chrono::days{1};
    const auto days = std::chrono::round<GameDuration>(config_.tuning.ocean.spin_up) / day;
    for (auto left = days; left > 0; --left) {
        ocean_.warm(grid_, detail::seasons(config_, time_ - (left * day)).now, Seconds{day}.count(),
                    nullptr);
    }
}

void Weather::spin_up_land() {
    const LandTuning& tuning = config_.tuning.land;
    constexpr GameDuration day = std::chrono::days{1};
    const auto days = std::chrono::round<GameDuration>(tuning.spin_up) / day;
    const auto counted = std::min<std::int64_t>(days, 365);
    const std::size_t size = grid_.size();
    std::vector<float> rain(size);
    std::vector<float> snow(size);
    std::vector<float> evaporation(size);
    std::vector<float> temperature(size);
    const std::vector<float> hail(size);
    std::vector<double> groundwater(size);
    std::vector<double> flow(size);
    land_.set_fields(grid_, LandFields{size});
    for (auto left = days; left > 0; --left) {
        const detail::Seasons seasons = detail::seasons(config_, time_ - (left * day));
        for (std::uint32_t index = 0; index < size; ++index) {
            const Ground& ground = ground_[index];
            const double surface =
                detail::targets(config_.tuning, ground, grid_.frame(index).up.z, seasons,
                                static_cast<double>(ocean_.fields().temperature[index]))
                    .surface;
            const auto falling = static_cast<double>(detail::usual_precipitation(ground));
            const double snowy =
                std::clamp((config_.tuning.water.snow_below - surface) / 3.0, 0.0, 1.0);
            temperature[index] = static_cast<float>(surface);
            rain[index] = static_cast<float>(falling * (1.0 - snowy));
            snow[index] = static_cast<float>(falling * snowy);
            evaporation[index] =
                static_cast<float>(tuning.drying * std::max(surface, 0.0) * land_.wetness(index));
        }
        land_.step(grid_,
                   LandWeather{.rain = rain,
                               .snow = snow,
                               .hail = hail,
                               .evaporation = evaporation,
                               .temperature = temperature,
                               .seconds = Seconds{day}.count()},
                   nullptr);
        if (left <= counted) {
            for (std::uint32_t index = 0; index < size; ++index) {
                groundwater[index] +=
                    land_.fields().groundwater[index] / static_cast<double>(counted);
                flow[index] +=
                    static_cast<double>(land_.fields().flow[index]) / static_cast<double>(counted);
            }
        }
    }
    LandFields settled = land_.fields();
    settled.usual_groundwater = std::move(groundwater);
    settled.usual_flow = std::move(flow);
    land_.set_fields(grid_, std::move(settled));
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
    const GameDuration sea_step = ocean_step(config_);
    const GameDuration sea_since =
        detail::last_step(at, sea_step) - detail::last_step(time_, sea_step);
    if (sea_since > GameDuration::zero()) {
        ocean_.step(grid_, fields_.wind, detail::seasons(config_, at).now,
                    Seconds{sea_since}.count(), &jobs);
    }
    const GameTime middle = at - (config_.step / 2);
    const detail::StepContext context{
        .config = &config_,
        .grid = &grid_,
        .ground = ground_,
        .sea = ocean_.fields().temperature,
        .slopes = slopes_,
        .systems = {},
        .calendar = &calendar_,
        .middle = middle,
        .seconds = Seconds{config_.step}.count(),
        .seasons = detail::seasons(config_, middle),
        .land = &land_,
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
    land_.step(grid_,
               LandWeather{.rain = fields_.rain,
                           .snow = fields_.snow,
                           .hail = fields_.hail,
                           .evaporation = fields_.evaporation,
                           .temperature = fields_.temperature,
                           .seconds = context.seconds},
               &jobs);
    time_ = at;
    balance_winds(&jobs);
}

void Weather::balance_winds(jobs::JobSystem* jobs) {
    const detail::StepContext context{
        .config = &config_,
        .grid = &grid_,
        .ground = ground_,
        .sea = ocean_.fields().temperature,
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
