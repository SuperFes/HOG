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
    std::vector<Soil> soil(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const planet::CellId cell = grid.cell(index);
        soil[index] = Soil{.biome = world.state(cell).biome};
    }
    return soil;
}

ForestConditions forest_conditions_of(const Weather& weather) {
    const WeatherFields& fields = weather.fields();
    return ForestConditions{.temperature = fields.recent_temperature,
                            .soil = weather.land().fields().soil,
                            .snow = weather.land().fields().snow,
                            .rain = fields.rain,
                            .rain_days = Seconds{weather.config().step}.count() / 86'400.0,
                            .storm = fields.storm,
                            .wind = fields.wind,
                            .groundwater = weather.land().fields().groundwater,
                            .usual_groundwater = weather.land().fields().usual_groundwater};
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

double dryness(double soil, double snow) noexcept {
    // mm of snow that keeps the ground from burning.
    constexpr double lying = 1.0;
    return snow >= lying ? 0.0 : std::clamp(1.0 - soil, 0.0, 1.0);
}

Forest::Forest(const ForestConfig& config, std::vector<Soil> soil, GameTime now)
    : config_(config), grid_(config_.level), soil_(std::move(soil)),
      time_(detail::last_step(now, positive(config_.step))),
      learning_until_(time_ + detail::years_of(config_.vegetation.dying.learning)) {
    HOG_ASSERT(config_.step > GameDuration::zero(), "forest step must be positive");
    HOG_ASSERT(soil_.size() == grid_.size(), "need soil for every cell");
    HOG_ASSERT(config_.planet_radius > 0.0, "planet radius must be positive");
    const std::uint32_t size = grid_.size();
    const double square_km = config_.planet_radius * config_.planet_radius / 1e6;
    areas_.reserve(size);
    fields_.biomass.reserve(size);
    fields_.dryness.resize(size);
    fields_.fire.resize(size);
    fields_.established.resize(size, 1.0F);
    fields_.failing.resize(size);
    fields_.dead.resize(size);
    fields_.charred.resize(size);
    fields_.shortfall.resize(size);
    fields_.accustomed.resize(size);
    fields_.scar.resize(size);
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

double Forest::living(std::uint32_t cell) const {
    const double holds = capacity(cell);
    return holds > 0.0 ? static_cast<double>(fields_.biomass[cell]) / holds : 1.0;
}

double Forest::sustainable(std::uint32_t cell) const {
    const DyingConfig& dying = config_.vegetation.dying;
    if (time_ < learning_until_) {
        return 1.0;
    }
    const double bearable = static_cast<double>(fields_.accustomed[cell]) + dying.tolerance;
    if (bearable >= 1.0) {
        return 1.0;
    }
    const double short_by =
        (static_cast<double>(fields_.shortfall[cell]) - bearable) / (1.0 - bearable);
    return 1.0 - (dying.worst * std::clamp(short_by, 0.0, 1.0));
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
    HOG_ASSERT(conditions.temperature.size() == size && conditions.soil.size() == size &&
                   conditions.snow.size() == size && conditions.rain.size() == size &&
                   conditions.storm.size() == size && conditions.wind.size() == size,
               "need conditions for every cell");
    HOG_ASSERT(
        (conditions.groundwater.empty() && conditions.usual_groundwater.empty()) ||
            (conditions.groundwater.size() == size && conditions.usual_groundwater.size() == size),
        "need ground water for every cell or none");
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
        fields_.dryness[cell] =
            static_cast<float>(dryness(static_cast<double>(conditions.soil[cell]),
                                       static_cast<double>(conditions.snow[cell])));
        feel(cell, conditions, days);
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
    const double before = living(cell);
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
        fields_.failing[cell] = 0.0F;
        settle(cell, before, living(cell), true, days);
        return;
    }
    // More than the ground water can keep dies back toward what it can; less grows toward it.
    const double keeps = sustainable(cell);
    if (before > keeps) {
        const double dying = std::exp(-days / (config_.vegetation.dying.years * days_per_year));
        biomass = static_cast<float>((keeps + ((before - keeps) * dying)) * holds);
    } else {
        const double temperature = static_cast<double>(conditions.temperature[cell]);
        const auto biome = static_cast<std::size_t>(soil_[cell].biome);
        biomass = static_cast<float>(
            std::min(grown(growth, holds, growth.years.at(biome), static_cast<double>(biomass),
                           speed(growth, temperature, static_cast<double>(fields_.dryness[cell])),
                           Seconds{config_.step}.count() / (86'400.0 * days_per_year)),
                     keeps * holds));
    }
    const double after = living(cell);
    fields_.failing[cell] = static_cast<float>(std::max(after - keeps, 0.0));
    settle(cell, before, after, false, days);
}

void Forest::feel(std::uint32_t cell, const ForestConditions& conditions, double days) {
    if (conditions.groundwater.empty()) {
        return;
    }
    const double usual = conditions.usual_groundwater[cell];
    // Under a millimetre or so, there's no ground water to speak of to fall short.
    const double felt =
        usual > 1.0 ? std::clamp(1.0 - (conditions.groundwater[cell] / usual), 0.0, 1.0) : 0.0;
    float& shortfall = fields_.shortfall[cell];
    const double toward = 1.0 - std::exp(-days / config_.vegetation.dying.days);
    shortfall = static_cast<float>(static_cast<double>(shortfall) +
                                   ((felt - static_cast<double>(shortfall)) * toward));
    // Learning, they take the worst of about the last year as usual, so what the ground water did
    // as a new world settled is forgotten; after, they grow used to worse only slowly.
    float& accustomed = fields_.accustomed[cell];
    if (time_ < learning_until_) {
        const double forgetting = 1.0 - std::exp(-days / days_per_year);
        accustomed = std::max(
            shortfall, static_cast<float>(static_cast<double>(accustomed) * (1.0 - forgetting)));
    } else if (shortfall > accustomed) {
        const double adapting =
            1.0 - std::exp(-days / (config_.vegetation.dying.adapting * days_per_year));
        accustomed = static_cast<float>(static_cast<double>(accustomed) +
                                        (static_cast<double>(shortfall - accustomed) * adapting));
    }
}

void Forest::settle(std::uint32_t cell, double before, double after, bool burned, double days) {
    const DyingConfig& dying = config_.vegetation.dying;
    double dead = static_cast<double>(fields_.dead[cell]);
    double charred = static_cast<double>(fields_.charred[cell]);
    if (after < before) {
        // What died stands dead; the burned share of the standing dead takes in what burned.
        const double lost = before - after;
        const double black = (charred * dead) + (burned ? lost : 0.0);
        dead += lost;
        charred = std::min(black / dead, 1.0);
    } else {
        // What grows back takes the places of the dead, so those fallen stay fallen.
        dead = std::max(dead - (after - before), 0.0);
    }
    dead = std::min(dead, std::max(1.0 - after, 0.0)) *
           std::exp(-days / (dying.standing * days_per_year));
    fields_.dead[cell] = static_cast<float>(dead);
    fields_.charred[cell] = dead > 0.0 ? static_cast<float>(charred) : 0.0F;

    float& scar = fields_.scar[cell];
    const double blackened =
        std::min(static_cast<double>(scar) + (burned ? std::max(before - after, 0.0) : 0.0), 1.0);
    scar = static_cast<float>(blackened *
                              std::exp(-days / (config_.vegetation.fire.scar * days_per_year)));

    float& established = fields_.established[cell];
    const auto old = static_cast<double>(established);
    established = static_cast<float>(
        after <= old
            ? after
            : old + ((after - old) * (1.0 - std::exp(-days / (dying.maturing * days_per_year)))));
}

void Forest::burned_ago(std::uint32_t cell, double years, double temperature, double dryness) {
    const GrowthConfig& growth = config_.vegetation.growth;
    const double holds = capacity(cell);
    if (holds <= 0.0) {
        return;
    }
    fields_.fire[cell] = 0.0F;
    fields_.failing[cell] = 0.0F;
    fields_.biomass[cell] = static_cast<float>(holds);
    const double before = living(cell);
    fields_.biomass[cell] = static_cast<float>(growth.seeds * holds);
    settle(cell, before, living(cell), true, 0.0);
    // Grown back a month at a time, as tend grows it.
    const double grows = speed(growth, temperature, dryness);
    const double years_to_grow = growth.years.at(static_cast<std::size_t>(soil_[cell].biome));
    constexpr double month = 1.0 / 12.0;
    for (double grown_for = 0.0; grown_for + (month / 2.0) < years; grown_for += month) {
        const double was = living(cell);
        fields_.biomass[cell] =
            static_cast<float>(grown(growth, holds, years_to_grow,
                                     static_cast<double>(fields_.biomass[cell]), grows, month));
        settle(cell, was, living(cell), false, month * days_per_year);
    }
}

double Forest::total() const {
    double sum = 0.0;
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        sum += static_cast<double>(fields_.biomass[cell]) * areas_[cell];
    }
    return sum * 1e6;
}

} // namespace hog::sim
