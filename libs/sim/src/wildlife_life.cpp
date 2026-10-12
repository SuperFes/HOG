#include "wildlife_stages.hpp"

#include <numbers>

namespace hog::sim::detail {

namespace {

// °C the recent temperature lies outside the species' comfort.
double outside(const Species& species, double temperature) {
    return std::max({species.cold - temperature, temperature - species.heat, 0.0});
}

bool breeding(const LifeContext& step, const Species& species, std::uint32_t cell) {
    // The seasons are reversed in the south.
    const double local =
        step.season_day + (step.grid->frame(cell).up.z < 0.0 ? days_per_year / 2.0 : 0.0);
    const double since =
        std::fmod(local - species.breeding.start + (2.0 * days_per_year), days_per_year);
    return since < species.breeding.days;
}

// What births are multiplied by: up to the cycle's boom as its peak passes.
double cycle(const LifeContext& step, const Species& species, std::uint32_t kind,
             std::uint32_t cell) {
    const Cycle& cycle = species.cycle;
    if (cycle.years <= 0.0) {
        return 1.0;
    }
    const auto peak = static_cast<double>(step.peaks[kind][cell]);
    double since = std::fmod(step.year - peak, cycle.years);
    if (since < 0.0) {
        since += cycle.years;
    }
    const double away = std::min(since, cycle.years - since);
    if (away >= cycle.width / 2.0) {
        return 1.0;
    }
    const double bump = 0.5 * (1.0 + std::cos(2.0 * std::numbers::pi * away / cycle.width));
    return 1.0 + ((cycle.boom - 1.0) * bump);
}

} // namespace

double room(const LifeContext& step, const Species& species, float count, std::uint32_t cell) {
    const Habitat& habitat = step.habitat[cell];
    const double capacity = species.crowded *
                            species.habitat.at(static_cast<std::size_t>(habitat.biome)) *
                            step.areas[cell];
    if (!passable(species, habitat) || capacity <= 0.0) {
        return 0.0;
    }
    return std::max(1.0 - (static_cast<double>(count) / capacity), 0.0);
}

void live(const LifeContext& step, WildlifeFields& fields, std::uint32_t cell) {
    const auto& species = step.config->ecology.species;
    const auto temperature = static_cast<double>(step.conditions.temperature[cell]);
    double dead_kg = 0.0;
    for (std::uint32_t kind = 0; kind < species.size(); ++kind) {
        const Species& one = species[kind];
        Population& population = fields.populations[kind];
        const auto count = static_cast<double>(population.count[cell]);
        if (count <= 0.0) {
            continue;
        }
        double dying = 1.0 / (one.lifespan * days_per_year);
        double born = 0.0;
        if (!dormant(one, temperature)) {
            const auto fed = static_cast<double>(population.fed[cell]);
            const double hunger = 1.0 - fed;
            dying += (one.starving * hunger * hunger) + (one.exposure * outside(one, temperature));
            if (breeding(step, one, cell)) {
                born = one.births / one.breeding.days * fed *
                       room(step, one, population.count[cell], cell) *
                       cycle(step, one, kind, cell) * step.days;
            }
        }
        const double dead = count * over(dying, step.days);
        dead_kg += dead * one.mass;
        population.count[cell] = static_cast<float>(count - dead + (count * born));
    }
    fields.carrion[cell] += static_cast<float>(dead_kg / step.areas[cell]);
}

double appeal(const LifeContext& step, const WildlifeFields& fields, std::uint32_t species,
              std::uint32_t cell) {
    const Species& one = step.config->ecology.species[species];
    const Habitat& habitat = step.habitat[cell];
    if (!passable(one, habitat)) {
        return 0.0;
    }
    constexpr double discomfort = 5.0; // °C outside its comfort that a cell seems a third as good
    const double comfort = std::exp(
        -outside(one, static_cast<double>(step.conditions.temperature[cell])) / discomfort);
    const double offered = on_offer(step, fields, species, cell);
    // A crowded cell still appeals a little, so a herd doesn't all leave its home at once.
    const double space =
        std::max(room(step, one, fields.populations[species].count[cell], cell), 0.1);
    return one.habitat.at(static_cast<std::size_t>(habitat.biome)) * comfort *
           (offered / (offered + one.half_fed)) * space;
}

} // namespace hog::sim::detail
