#include "wildlife_stages.hpp"

#include <hog/sim/weather_tuning.hpp>

#include <cstddef>

namespace hog::sim::detail {

namespace {

constexpr std::size_t forage_slot = 0;
constexpr std::size_t carrion_slot = 1;
constexpr std::size_t first_prey_slot = 2;

std::size_t slot(const Food& food) {
    switch (food.kind) {
    case Food::Kind::Forage:
        return forage_slot;
    case Food::Kind::Carrion:
        return carrion_slot;
    case Food::Kind::Prey:
        return first_prey_slot + food.prey;
    }
    return forage_slot;
}

// kg of each food in the cell that can be got at, in slot order.
std::vector<double> available(const LifeContext& step, const WildlifeFields& fields,
                              std::uint32_t cell) {
    const auto& species = step.config->ecology.species;
    const double area = step.areas[cell];
    const auto temperature = static_cast<double>(step.conditions.temperature[cell]);
    std::vector<double> kg(first_prey_slot + species.size());
    kg[forage_slot] = static_cast<double>(fields.forage[cell]) * area;
    kg[carrion_slot] = static_cast<double>(fields.carrion[cell]) * area;
    for (std::size_t prey = 0; prey < species.size(); ++prey) {
        // Sleeping animals are hidden away.
        kg[first_prey_slot + prey] =
            dormant(species[prey], temperature)
                ? 0.0
                : static_cast<double>(fields.populations[prey].count[cell]) * species[prey].mass;
    }
    return kg;
}

double offer(const Species& species, std::span<const double> kg, double area) {
    double total = 0.0;
    for (const Food& food : species.diet) {
        total += food.preference * kg[slot(food)] / area;
    }
    return total;
}

// Growth from 0 to 1 as the recent weather is warm and wet enough.
double growing(const ForageConfig& forage, const Habitat& habitat, double temperature,
               double precipitation) {
    const double warmth =
        std::clamp((temperature - forage.cold) / (forage.warm - forage.cold), 0.0, 1.0);
    const double water =
        habitat.ocean ? 1.0 : precipitation / (std::max(precipitation, 0.0) + forage.wet);
    return warmth * std::clamp(water, 0.0, 1.0);
}

} // namespace

double on_offer(const LifeContext& step, const WildlifeFields& fields, std::uint32_t species,
                std::uint32_t cell) {
    return offer(step.config->ecology.species[species], available(step, fields, cell),
                 step.areas[cell]);
}

void grow(const LifeContext& step, WildlifeFields& fields, std::uint32_t cell) {
    const ForageConfig& forage = step.config->ecology.forage;
    const Habitat& habitat = step.habitat[cell];
    const auto temperature = static_cast<double>(step.conditions.temperature[cell]);
    const double target = forage.capacity.at(static_cast<std::size_t>(habitat.biome)) *
                          growing(forage, habitat, temperature,
                                  static_cast<double>(step.conditions.precipitation[cell]));
    const double seconds = step.days * 86'400.0;
    const auto now = static_cast<double>(fields.forage[cell]);
    const Seconds response = target > now ? forage.growth : forage.wither;
    fields.forage[cell] =
        static_cast<float>(now + ((target - now) * (1.0 - std::exp(-seconds / response.count()))));

    // Rot slows almost to nothing in the frost.
    constexpr double reference = 20.0;
    const double warmth = std::clamp((temperature + 10.0) / (reference + 10.0), 0.05, 2.0);
    fields.carrion[cell] *= static_cast<float>(std::exp(-seconds * warmth / forage.rot.count()));
}

void feed(const LifeContext& step, WildlifeFields& fields, std::uint32_t cell) {
    const auto& species = step.config->ecology.species;
    const double area = step.areas[cell];
    const auto temperature = static_cast<double>(step.conditions.temperature[cell]);
    const std::vector<double> kg = available(step, fields, cell);

    // What each species asks of each of its foods, then how far there's enough to go round.
    std::vector<std::vector<double>> wants(species.size());
    std::vector<double> asked(kg.size());
    std::vector<double> demand(species.size());
    std::vector<double> full(species.size());
    for (std::size_t eater = 0; eater < species.size(); ++eater) {
        const Species& kind = species[eater];
        const double offered = offer(kind, kg, area);
        full[eater] = offered / (offered + kind.half_fed);
        demand[eater] =
            kind.appetite * static_cast<double>(fields.populations[eater].count[cell]) * step.days;
        if (demand[eater] <= 0.0 || offered <= 0.0 || dormant(kind, temperature)) {
            continue;
        }
        wants[eater].resize(kind.diet.size());
        for (std::size_t item = 0; item < kind.diet.size(); ++item) {
            const Food& food = kind.diet[item];
            const double share = food.preference * kg[slot(food)] / area / offered;
            // Hunters kill more than they eat; the rest is left for scavengers.
            const double wasted = food.kind == Food::Kind::Prey ? 1.0 - kind.leftovers : 1.0;
            wants[eater][item] = demand[eater] * full[eater] * share / wasted;
            asked[slot(food)] += wants[eater][item];
        }
    }
    std::vector<double> granted(kg.size(), 1.0);
    const double most = over(step.config->most_eaten, step.days);
    for (std::size_t food = 0; food < kg.size(); ++food) {
        if (asked[food] > 0.0) {
            granted[food] = std::min(1.0, most * kg[food] / asked[food]);
        }
    }

    std::vector<double> taken(kg.size());
    double left_over = 0.0;
    for (std::size_t eater = 0; eater < species.size(); ++eater) {
        const Species& kind = species[eater];
        Population& population = fields.populations[eater];
        if (dormant(kind, temperature)) {
            population.fed[cell] = 1.0F;
            continue;
        }
        double eaten = 0.0;
        for (std::size_t item = 0; item < wants[eater].size(); ++item) {
            const Food& food = kind.diet[item];
            const double got = wants[eater][item] * granted[slot(food)];
            taken[slot(food)] += got;
            const double kept = food.kind == Food::Kind::Prey ? 1.0 - kind.leftovers : 1.0;
            eaten += got * kept;
            left_over += got * (1.0 - kept);
        }
        population.fed[cell] = static_cast<float>(
            demand[eater] > 0.0 ? std::min(eaten / demand[eater], 1.0) : full[eater]);
    }

    fields.forage[cell] = static_cast<float>(
        std::max(static_cast<double>(fields.forage[cell]) - (taken[forage_slot] / area), 0.0));
    fields.carrion[cell] = static_cast<float>(std::max(
        static_cast<double>(fields.carrion[cell]) + ((left_over - taken[carrion_slot]) / area),
        0.0));
    for (std::size_t prey = 0; prey < species.size(); ++prey) {
        float& count = fields.populations[prey].count[cell];
        count = static_cast<float>(std::max(
            static_cast<double>(count) - (taken[first_prey_slot + prey] / species[prey].mass),
            0.0));
    }
}

} // namespace hog::sim::detail
