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
    // Multiplies the width and depth rivers would have on Earth for the water they carry.
    double river_scale = 1.0;
    // How many plates the crust is broken into.
    int plates = 10;
    // Metres between the cells rivers wear the land on: finer wears finer valleys, and takes
    // longer and more memory to evolve. Cells are a power of two to a cube face, so this is the
    // most they're made, and never finer than a 1,024th of a face.
    double erosion_cell = 150.0;
    // One turn of day and night, in real time; the year is Earth's.
    GameDuration day_length = std::chrono::hours{4};
    double axial_tilt = 23.44; // degrees
    // Where the world's landform is kept once evolved, so it evolves once rather than whenever
    // the world is built; HOG_BAKES names one where this is empty, and with neither it's never
    // kept. Not part of the world, so never read from its config.
    std::filesystem::path bakes{};
};

// Identifies a world by what shapes its ground, so state saved for one is never loaded into
// another. Day length and tilt only move the sun, so they can change under a running world. A
// river scale of 1, ten plates and 150 m erosion cells leave the id as it was before those could
// be set.
[[nodiscard]] std::uint64_t world_id(const PlanetConfig& config) noexcept;

// Reads a [planet] table:
//
//     [planet]
//     seed = 1
//     ocean_fraction = 0.7      # optional
//     population = 500_000      # or radius_km = 115.0, not both
//     settled_density = 30.0    # optional, with population
//     wilderness_factor = 3.0   # optional, with population
//     river_scale = 1.0         # optional, above 0 and at most 20
//     plates = 10               # optional, 2 to 64
//     erosion_cell = 150.0      # optional, metres, 50 to 5000
//     day_hours = 4.0           # optional
//     axial_tilt = 23.44        # optional, degrees
//
// Unknown keys are errors, so typos don't silently fall back to defaults.
[[nodiscard]] Result<PlanetConfig> parse_planet_config(std::string_view text);
[[nodiscard]] Result<PlanetConfig> load_planet_config(const std::filesystem::path& path);

} // namespace hog::planet
