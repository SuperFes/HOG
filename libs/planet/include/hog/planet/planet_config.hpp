#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace hog::planet {

// Inputs to the population sizing rule in docs/plan.md.
struct PopulationSizing {
    double population = 0.0;
    double settled_density = 30.0;  // people per km² of settled land
    double wilderness_factor = 3.0; // total land per unit of settled land
};

// Radius in metres of a planet with enough land for the population.
[[nodiscard]] double radius_for_population(const PopulationSizing& sizing, double ocean_fraction);

struct PlanetConfig {
    std::uint64_t seed = 0;
    double ocean_fraction = 0.7;
    double radius = 0.0; // metres
    // One turn of day and night, in real time; the year is Earth's.
    GameDuration day_length = std::chrono::hours{4};
    double axial_tilt = 23.44; // degrees
};

// Reads a [planet] table:
//
//     [planet]
//     seed = 1
//     ocean_fraction = 0.7      # optional
//     population = 500_000      # or radius_km = 115.0, not both
//     settled_density = 30.0    # optional, with population
//     wilderness_factor = 3.0   # optional, with population
//     day_hours = 4.0           # optional
//     axial_tilt = 23.44        # optional, degrees
//
// Unknown keys are errors, so typos don't silently fall back to defaults.
[[nodiscard]] Result<PlanetConfig> parse_planet_config(std::string_view text);
[[nodiscard]] Result<PlanetConfig> load_planet_config(const std::filesystem::path& path);

} // namespace hog::planet
