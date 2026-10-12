#include "cells.hpp"

#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/sim/ecology.hpp>
#include <hog/sim/forest.hpp>
#include <hog/sim/weather.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace hog::sim {

namespace {

GameDuration positive(GameDuration duration) {
    return duration > GameDuration::zero() ? duration : GameDuration{1};
}

// Logistic growth over `years` at full speed, from the seeds a fire leaves to 90% of capacity.
double grown(const GrowthConfig& growth, double capacity, double years_to_grow, double biomass,
             double speed, double years) {
    const double least = growth.seeds * capacity;
    const double from = std::max(biomass, least);
    const double rate = std::log(9.0 * (1.0 - growth.seeds) / growth.seeds) / years_to_grow;
    return capacity / (1.0 + (((capacity / from) - 1.0) * std::exp(-rate * speed * years)));
}

// How fast plants grow, 0 to 1, as it's warm enough and the ground wet enough.
double speed(const GrowthConfig& growth, double temperature, double dryness) {
    const double warmth =
        std::clamp((temperature - growth.cold) / (growth.warm - growth.cold), 0.0, 1.0);
    return warmth * std::clamp((1.0 - dryness) / growth.wet, 0.0, 1.0);
}

} // namespace

std::vector<Soil> soil_for(const CellGrid& grid, const planet::World& world) {
    const planet::Climate& climate = world.geology().climate();
    std::vector<Soil> soil(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const planet::CellId cell = grid.cell(index);
        soil[index] = Soil{
            .biome = world.state(cell).biome,
            .moisture = static_cast<float>(
                std::clamp(planet::fixed_to_double(climate.moisture(cell)), 0.0, 1.0)),
        };
    }
    return soil;
}

ForestConditions forest_conditions_of(const Weather& weather) {
    const WeatherFields& fields = weather.fields();
    return ForestConditions{.temperature = fields.recent_temperature,
                            .precipitation = fields.recent_precipitation,
                            .rain = fields.rain,
                            .rain_days = Seconds{weather.config().step}.count() / 86'400.0,
                            .storm = fields.storm,
                            .wind = fields.wind};
}

Stage stage_of(double biomass, double capacity) noexcept {
    if (capacity <= 0.0) {
        return Stage::Bare;
    }
    const double share = biomass / capacity;
    if (share < 0.1) {
        return Stage::Bare;
    }
    if (share < 0.25) {
        return Stage::Grass;
    }
    if (share < 0.5) {
        return Stage::Shrub;
    }
    return share < 0.9 ? Stage::Young : Stage::Mature;
}

std::string_view to_string(Stage stage) noexcept {
    switch (stage) {
    case Stage::Bare:
        return "bare";
    case Stage::Grass:
        return "grass";
    case Stage::Shrub:
        return "shrub";
    case Stage::Young:
        return "young";
    case Stage::Mature:
        return "mature";
    }
    return "unknown";
}

double dryness(const DrynessConfig& config, double moisture, double temperature,
               double precipitation) noexcept {
    const double supply = std::max(precipitation, 0.0);
    const double demand = config.demand * std::max(temperature, 0.0);
    const double kept = supply + demand > 0.0 ? supply / (supply + demand) : 1.0;
    const double wetness =
        (config.climate * std::clamp(moisture, 0.0, 1.0)) + ((1.0 - config.climate) * kept);
    return std::clamp(1.0 - wetness, 0.0, 1.0);
}

Forest::Forest(const ForestConfig& config, std::vector<Soil> soil, GameTime now)
    : config_(config), grid_(config_.level), soil_(std::move(soil)),
      time_(detail::last_step(now, positive(config_.step))) {
    HOG_ASSERT(config_.step > GameDuration::zero(), "forest step must be positive");
    HOG_ASSERT(soil_.size() == grid_.size(), "need soil for every cell");
    HOG_ASSERT(config_.planet_radius > 0.0, "planet radius must be positive");
    const std::uint32_t size = grid_.size();
    const double square_km = config_.planet_radius * config_.planet_radius / 1e6;
    areas_.reserve(size);
    fields_.biomass.reserve(size);
    fields_.dryness.resize(size);
    fields_.fire.resize(size);
    catching_.resize(size);
    for (std::uint32_t cell = 0; cell < size; ++cell) {
        areas_.push_back(grid_.area(cell) * square_km);
        fields_.biomass.push_back(static_cast<float>(capacity(cell)));
    }
}

double Forest::capacity(std::uint32_t cell) const {
    return config_.vegetation.growth.capacity.at(static_cast<std::size_t>(soil_[cell].biome));
}

Stage Forest::stage(std::uint32_t cell) const {
    return stage_of(static_cast<double>(fields_.biomass[cell]), capacity(cell));
}

double Forest::flammability(std::uint32_t cell) const {
    return flammability(cell, static_cast<double>(fields_.biomass[cell]));
}

double Forest::flammability(std::uint32_t cell, double biomass) const {
    const FireConfig& fire = config_.vegetation.fire;
    const double holds = capacity(cell);
    const double burnable = biomass - (config_.vegetation.growth.seeds * holds);
    if (burnable < fire.out * holds || burnable <= 0.0) {
        return 0.0;
    }
    const double dry = std::clamp((static_cast<double>(fields_.dryness[cell]) - fire.damp) /
                                      (fire.parched - fire.damp),
                                  0.0, 1.0);
    return dry * burnable / (burnable + fire.fuel);
}

std::uint32_t Forest::burning() const {
    return static_cast<std::uint32_t>(
        std::ranges::count_if(fields_.fire, [](float fire) { return fire > 0.0F; }));
}

bool Forest::ignite(std::uint32_t cell) {
    const double flammable = flammability(cell);
    if (flammable < config_.vegetation.fire.out) {
        return false;
    }
    float& fire = fields_.fire[cell];
    if (fire <= 0.0F) {
        ++fires_.started;
        fires_.alight += areas_[cell];
    }
    fire = std::max(fire, static_cast<float>(flammable));
    return true;
}

FireReport Forest::drain_fires() {
    return std::exchange(fires_, FireReport{});
}

std::uint64_t Forest::advance_to(GameTime now, const ForestConditions& conditions,
                                 jobs::JobSystem& jobs) {
    const std::size_t size = grid_.size();
    HOG_ASSERT(conditions.temperature.size() == size && conditions.precipitation.size() == size &&
                   conditions.rain.size() == size && conditions.storm.size() == size &&
                   conditions.wind.size() == size,
               "need conditions for every cell");
    std::uint64_t steps = 0;
    while (next_step() <= now) {
        step(next_step(), conditions, jobs);
        ++steps;
    }
    return steps;
}

void Forest::step(GameTime at, const ForestConditions& conditions, jobs::JobSystem& jobs) {
    const double days = Seconds{config_.step}.count() / 86'400.0;
    const auto step_number = static_cast<std::uint64_t>(at.time_since_epoch() / config_.step);
    detail::for_cells(&jobs, grid_.size(), [&](std::uint32_t cell) {
        fields_.dryness[cell] = static_cast<float>(
            dryness(config_.vegetation.dryness, static_cast<double>(soil_[cell].moisture),
                    static_cast<double>(conditions.temperature[cell]),
                    static_cast<double>(conditions.precipitation[cell])));
    });
    detail::for_cells(&jobs, grid_.size(), [&](std::uint32_t cell) {
        catching_[cell] = catching(cell, conditions, days, step_number);
    });
    detail::for_cells(&jobs, grid_.size(),
                      [&](std::uint32_t cell) { tend(cell, conditions, days); });
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        if (catching_[cell] != Catching::No) {
            fires_.started += catching_[cell] == Catching::Lightning ? 1U : 0U;
            fires_.alight += areas_[cell];
        }
    }
    time_ = at;
}

Forest::Catching Forest::catching(std::uint32_t cell, const ForestConditions& conditions,
                                  double days, std::uint64_t step_number) const {
    const FireConfig& fire = config_.vegetation.fire;
    if (fields_.fire[cell] > 0.0F) {
        return Catching::No;
    }
    const double flammable = flammability(cell);
    if (flammable < fire.out) {
        return Catching::No;
    }
    // Fire runs with the wind: what blows from a burning neighbour into this cell carries it.
    const DVec2 wind{conditions.wind[cell]};
    double neighbours = 0.0;
    for (std::size_t k = 0; k < 4; ++k) {
        const auto burning = static_cast<double>(fields_.fire[grid_.neighbors(cell)[k]]);
        if (burning > 0.0) {
            const DVec2 towards = -glm::normalize(grid_.offsets(cell)[k]);
            neighbours += burning * std::exp(fire.wind * glm::dot(wind, towards));
        }
    }
    const double struck = fire.lightning * static_cast<double>(conditions.storm[cell]);
    const double spread = fire.spread * neighbours;
    const double roll = detail::fraction(hash_values(config_.seed, step_number, cell));
    if (roll < 1.0 - std::exp(-days * flammable * struck)) {
        return Catching::Lightning;
    }
    return roll < 1.0 - std::exp(-days * flammable * (struck + spread)) ? Catching::Spread
                                                                        : Catching::No;
}

void Forest::tend(std::uint32_t cell, const ForestConditions& conditions, double days) {
    const GrowthConfig& growth = config_.vegetation.growth;
    const FireConfig& fire = config_.vegetation.fire;
    const double holds = capacity(cell);
    float& biomass = fields_.biomass[cell];
    float& burning = fields_.fire[cell];
    if (holds <= 0.0) {
        biomass = 0.0F;
        burning = 0.0F;
        return;
    }
    if (catching_[cell] != Catching::No) {
        burning = static_cast<float>(flammability(cell));
        return;
    }
    if (burning > 0.0F) {
        const double least = growth.seeds * holds;
        const double burn = std::log((1.0 - growth.seeds) / fire.out) / fire.burn_days;
        const double left =
            least + ((static_cast<double>(biomass) - least) * std::exp(-burn * days));
        biomass = static_cast<float>(left);
        const double rain = conditions.rain_days > 0.0
                                ? static_cast<double>(conditions.rain[cell]) / conditions.rain_days
                                : 0.0;
        const double now =
            std::min(static_cast<double>(burning) * std::exp(-days * rain / fire.douse),
                     flammability(cell, left));
        burning = now < fire.out ? 0.0F : static_cast<float>(now);
        // Burnt out, it leaves only the seeds.
        if (burning == 0.0F && left - least < fire.out * holds) {
            biomass = static_cast<float>(least);
        }
        return;
    }
    const auto temperature = static_cast<double>(conditions.temperature[cell]);
    const auto biome = static_cast<std::size_t>(soil_[cell].biome);
    biomass = static_cast<float>(
        grown(growth, holds, growth.years.at(biome), static_cast<double>(biomass),
              speed(growth, temperature, static_cast<double>(fields_.dryness[cell])),
              Seconds{config_.step}.count() / (86'400.0 * days_per_year)));
}

double Forest::total() const {
    double sum = 0.0;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        sum += static_cast<double>(fields_.biomass[cell]) * areas_[cell];
    }
    return sum * 1e6;
}

} // namespace hog::sim
