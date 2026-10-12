#pragma once

#include <hog/core/result.hpp>
#include <hog/planet/climate.hpp>

#include <array>
#include <filesystem>
#include <string_view>

namespace hog::sim {

// How plants grow back towards what each biome holds.
struct GrowthConfig {
    // kg per m² of living plants each biome holds fully grown.
    std::array<double, planet::biome_count> capacity{};
    // Years each biome takes to grow back from seeds to most of its capacity, growing fully.
    std::array<double, planet::biome_count> years = [] {
        std::array<double, planet::biome_count> every{};
        every.fill(30.0);
        return every;
    }();
    double seeds = 0.02; // share of its capacity a cell keeps through a fire, to grow back from
    double cold = 0.0;   // °C below which nothing grows
    double warm = 15.0;  // °C from which growth is full
    double wet = 0.5;    // wetness (1 less dryness) from which growth is full
};

// How dry the ground is, from the planet's moisture map and the recent weather.
struct DrynessConfig {
    double demand = 0.15; // mm per day the air draws from the ground per °C above freezing
    double climate = 0.5; // the moisture map's share, against the recent weather's
};

struct FireConfig {
    double damp = 0.5;    // dryness below which nothing burns
    double parched = 0.9; // dryness from which plants burn freely
    double fuel = 0.5;    // kg per m² of burnable biomass for half flammability
    // Per day, how readily a full thunderstorm lights a cell that burns freely.
    double lightning = 0.05;
    // Per day, how readily a cell burning throughout lights a neighbour that burns freely, in
    // still air. With the wind it's multiplied by exp(wind × speed in m/s), and against it divided.
    double spread = 1.5;
    double wind = 0.15;
    double burn_days = 2.0; // for a fire to burn its cell down to the seeds
    double douse = 5.0;     // mm per day of rain that puts most of a fire out within a day
    // A fire goes out burning less than this share of its cell, or with less than this share of
    // its cell's plants left to burn.
    double out = 0.05;
};

// How the planet's plants grow and burn.
struct Vegetation {
    GrowthConfig growth{};
    DrynessConfig dryness{};
    FireConfig fire{};
};

// Reads vegetation from TOML, where missing keys keep their defaults:
//
//     [growth]
//     capacity = { temperate_forest = 20, grassland = 0.5 }   # kg per m²
//     years = { temperate_forest = 60, grassland = 1 }
//     seeds = 0.02
//     cold = 0
//     warm = 15
//     wet = 0.5
//
//     [dryness]
//     demand = 0.15
//     climate = 0.5
//
//     [fire]
//     damp = 0.5
//     parched = 0.9
//     fuel = 0.5
//     lightning = 0.05
//     spread = 1.5
//     wind = 0.15
//     burn_days = 2
//     douse = 5
//     out = 0.05
//
// Unknown tables, keys and biomes are errors.
[[nodiscard]] Result<Vegetation> parse_vegetation(std::string_view text);
[[nodiscard]] Result<Vegetation> load_vegetation(const std::filesystem::path& path);

} // namespace hog::sim
