#include "cells.hpp"

#include <hog/core/assert.hpp>
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
    return ForestConditions{.temperature = weather.fields().recent_temperature,
                            .precipitation = weather.fields().recent_precipitation};
}

Stage stage_of(double biomass, double capacity) noexcept {
    if (capacity <= 0.0) {
        return Stage::Bare;
    }
    const double share = biomass / capacity;
    if (share < 0.05) {
        return Stage::Bare;
    }
    if (share < 0.2) {
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

std::uint64_t Forest::advance_to(GameTime now, const ForestConditions& conditions,
                                 jobs::JobSystem& jobs) {
    HOG_ASSERT(conditions.temperature.size() == grid_.size() &&
                   conditions.precipitation.size() == grid_.size(),
               "need conditions for every cell");
    std::uint64_t steps = 0;
    while (next_step() <= now) {
        step(next_step(), conditions, jobs);
        ++steps;
    }
    return steps;
}

void Forest::step(GameTime at, const ForestConditions& conditions, jobs::JobSystem& jobs) {
    const GrowthConfig& growth = config_.vegetation.growth;
    const double years = Seconds{config_.step}.count() / (86'400.0 * days_per_year);
    detail::for_cells(&jobs, grid_.size(), [&](std::uint32_t cell) {
        const auto temperature = static_cast<double>(conditions.temperature[cell]);
        const double dry =
            dryness(config_.vegetation.dryness, static_cast<double>(soil_[cell].moisture),
                    temperature, static_cast<double>(conditions.precipitation[cell]));
        fields_.dryness[cell] = static_cast<float>(dry);
        const double holds = capacity(cell);
        float& biomass = fields_.biomass[cell];
        if (holds <= 0.0) {
            biomass = 0.0F;
            return;
        }
        const auto biome = static_cast<std::size_t>(soil_[cell].biome);
        biomass = static_cast<float>(grown(growth, holds, growth.years.at(biome),
                                           static_cast<double>(biomass),
                                           speed(growth, temperature, dry), years));
    });
    time_ = at;
}

double Forest::total() const {
    double sum = 0.0;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        sum += static_cast<double>(fields_.biomass[cell]) * areas_[cell];
    }
    return sum * 1e6;
}

} // namespace hog::sim
