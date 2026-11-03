#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/vents.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>

namespace hog::globe {
namespace {

// The scatter is by cell, at a level whose cells are about a kilometre or two across.
constexpr int scatter_level = 7;
// Earth's heat flow, W/m².
constexpr double earth_heat_flow = 0.087;
constexpr std::uint64_t vent_salt = 0x76656E74ULL;

double unit(std::uint64_t value) {
    return static_cast<double>(value >> 11U) * 0x1.0p-53;
}

} // namespace

std::vector<VentSite> vent_sites(const Generator& generator) {
    const Planet& planet = generator.planet();
    const double ratio = planet.heat_flow / earth_heat_flow;
    const double area_km2 = 4.0 * std::numbers::pi * planet.radius * planet.radius * 1e-6;
    const std::uint64_t cells = 6ULL << (2 * scatter_level);
    const double chance = vents_per_km2 * ratio * ratio * area_km2 / static_cast<double>(cells);
    const double sea = generator.sea_level().value_or(0.0);
    std::vector<VentSite> sites;
    for (std::uint64_t index = 0; index < cells; ++index) {
        const std::uint64_t key = hash_values(planet.seed, vent_salt, index);
        if (unit(key) >= chance) {
            continue;
        }
        const DVec3 direction = center(cell_at_index(scatter_level, index));
        const auto ground = ground_below(generator, direction, 0.5);
        // Land only: a vent under the sea is the sea's, not ours.
        if (ground && (!generator.sea_level() || *ground > sea)) {
            sites.push_back({.direction = direction, .ground = *ground, .key = key});
        }
    }
    return sites;
}

} // namespace hog::globe
