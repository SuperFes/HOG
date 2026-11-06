#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/vents.hpp>

#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <optional>
#include <cstdint>
#include <numbers>

namespace hog::globe {
namespace {

// The scatter is by cell, at a level whose cells are a few kilometres across, so a volcano and
// its warp stay within the cells around its own.
constexpr int scatter_level = 6;
// Earth's heat flow, W/m².
constexpr double earth_heat_flow = 0.087;
constexpr std::uint64_t vent_salt = 0x76656E74ULL;
constexpr std::uint64_t pool_salt = 0x706F6F6CULL;
// How far under the ground a pool's cave is sought, and the tries each candidate gets.
constexpr double pool_reach = 60.0;
constexpr int pool_tries = 4;
// A cave lower than this is no place for a pool.
constexpr double pool_headroom = 2.0;
constexpr double pool_step = 0.5;

double unit(std::uint64_t value) {
    return static_cast<double>(value >> 11U) * 0x1.0p-53;
}

} // namespace

std::optional<Volcano> volcano_in(const Planet& planet, Cell cell) {
    const double ratio = planet.heat_flow / earth_heat_flow;
    const double area_km2 = 4.0 * std::numbers::pi * planet.radius * planet.radius * 1e-6;
    const std::uint64_t cells = 6ULL << (2 * scatter_level);
    const double chance = vents_per_km2 * ratio * ratio * area_km2 / static_cast<double>(cells);
    const std::uint64_t key = hash_values(planet.seed, vent_salt, cell.face, cell.i, cell.j);
    if (cell.level != scatter_level || unit(key) >= chance) {
        return std::nullopt;
    }
    const double side = std::sqrt(4.0 * std::numbers::pi / 6.0) * planet.radius /
                        static_cast<double>(1 << scatter_level);
    const DVec3 middle = center(cell);
    const DVec3 side_axis = std::abs(middle.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(side_axis, middle));
    const DVec3 north = glm::cross(middle, east);
    Volcano volcano;
    volcano.key = key;
    volcano.extinct = unit(hash_values(key, 14)) < extinct_share;
    volcano.centre = glm::normalize(
        middle + ((((unit(hash_values(key, 1)) - 0.5) * east) +
                   ((unit(hash_values(key, 2)) - 0.5) * north)) *
                  (0.8 * side / planet.radius)));
    volcano.radius = 0.2 * side * (1.0 + unit(hash_values(key, 3)));
    // Flanks of about sixteen degrees, steeper near the top.
    volcano.height = volcano.radius * (0.2 + (0.2 * unit(hash_values(key, 4))));
    volcano.crater = 0.08 + (0.08 * unit(hash_values(key, 5)));
    volcano.depth = volcano.height * (0.2 + (0.2 * unit(hash_values(key, 6))));
    constexpr std::array<double, 3> widest{0.25, 0.15, 0.08};
    for (std::size_t k = 0; k < 3; ++k) {
        volcano.amplitude[k] = widest[k] * (0.4 + (0.6 * unit(hash_values(key, 7 + k))));
        volcano.phase[k] = 2.0 * std::numbers::pi * unit(hash_values(key, 10 + k));
    }
    return volcano;
}

std::vector<VentSite> vent_sites(const Generator& generator) {
    const Planet& planet = generator.planet();
    const std::uint64_t cells = 6ULL << (2 * scatter_level);
    const double sea = generator.sea_level().value_or(0.0);
    std::vector<VentSite> sites;
    for (std::uint64_t index = 0; index < cells; ++index) {
        const auto volcano = volcano_in(planet, cell_at_index(scatter_level, index));
        if (!volcano || volcano->extinct) {
            continue;
        }
        const auto ground = ground_below(generator, volcano->centre, 0.5);
        // Land only: a vent under the sea is the sea's, not ours.
        if (ground && (!generator.sea_level() || *ground > sea)) {
            sites.push_back({.direction = volcano->centre, .ground = *ground, .key = volcano->key});
        }
    }
    // Pools on cavern floors, in whatever rock holds a cave open near the surface.
    const double ratio = planet.heat_flow / earth_heat_flow;
    const double area_km2 = 4.0 * std::numbers::pi * planet.radius * planet.radius * 1e-6;
    const double chance = pools_per_km2 * ratio * ratio * area_km2 / static_cast<double>(cells);
    const double side = std::sqrt(4.0 * std::numbers::pi / 6.0) * planet.radius /
                        static_cast<double>(1 << scatter_level);
    for (std::uint64_t index = 0; index < cells; ++index) {
        const Cell cell = cell_at_index(scatter_level, index);
        const std::uint64_t key = hash_values(planet.seed, pool_salt, index);
        if (unit(key) >= chance) {
            continue;
        }
        const DVec3 middle = center(cell);
        const DVec3 side_axis =
            std::abs(middle.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
        const DVec3 east = glm::normalize(glm::cross(side_axis, middle));
        const DVec3 north = glm::cross(middle, east);
        for (int attempt = 0; attempt < pool_tries; ++attempt) {
            const std::uint64_t own = hash_values(key, attempt);
            const DVec3 direction = glm::normalize(
                middle + ((((unit(hash_values(own, 1)) - 0.5) * east) +
                           ((unit(hash_values(own, 2)) - 0.5) * north)) *
                          (side / planet.radius)));
            const Generator::Column column = generator.column(direction, 0.5);
            if (column.ground() <= sea) {
                continue;
            }
            bool found = false;
            for (const auto& [bottom, top] :
                 column.caves(column.ground() - pool_reach, column.ground(), 0.0)) {
                // The stretch only may hold a cave: the floor is where it first opens from solid.
                bool solid = false;
                for (double at = bottom - pool_step; at + pool_headroom < top && !found;
                     at += pool_step) {
                    const bool open = column.sample(at).distance > 0.0;
                    if (open && solid && column.sample(at + pool_headroom).distance > 0.0) {
                        sites.push_back({.direction = direction, .ground = at, .pool = true,
                                         .key = own});
                        found = true;
                    }
                    solid = !open;
                }
                if (found) {
                    break;
                }
            }
            if (found) {
                break;
            }
        }
    }
    return sites;
}

} // namespace hog::globe
