#pragma once

#include <hog/core/result.hpp>
#include <hog/planet/climate.hpp>
#include <hog/sim/weather_tuning.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hog::sim {

inline constexpr double days_per_year = 365.2422;

// Plants that grazers eat, growing back towards what the ground and the weather allow, and the
// carrion that scavengers eat.
struct ForageConfig {
    // kg per km² each biome holds when it's warm and wet enough.
    std::array<double, planet::biome_count> capacity{};
    Seconds growth = std::chrono::days{30}; // to grow most of the way back
    Seconds wither = std::chrono::days{90}; // to die back once conditions turn
    double cold = 0.0;                      // °C below which nothing grows
    double warm = 15.0;                     // °C from which growth is full
    double wet = 1.0;                       // mm per day of precipitation for half growth on land
    // Carrion rots most of the way over this at 20 °C, and slower in the cold.
    Seconds rot = std::chrono::days{10};
};

enum class Moves : std::uint8_t { Land, Water, Air };

struct Food {
    enum class Kind : std::uint8_t { Forage, Carrion, Prey };
    Kind kind = Kind::Forage;
    std::uint32_t prey = 0; // the species eaten, for Prey
    double preference = 1.0;
};

// Days after the local spring equinox, so breeding falls in the same season in either hemisphere.
struct Breeding {
    double start = 0.0;
    double days = days_per_year;
};

// An authored boom every `years`, peaking in a different year in each region.
struct Cycle {
    double years = 0.0; // none when 0
    double boom = 1.0;  // births are multiplied by this at the peak
    double width = 1.0; // years a boom lasts
    int region_level = 2;
};

// How a species comes back once it's gone from a region, or from everywhere.
struct Recovery {
    // What brings it back once none are left anywhere.
    enum class After : std::uint8_t {
        Admin,     // nothing: it waits to be reintroduced
        Relict,    // a hidden few turn up where the ground is best
        Successor, // another species takes its place
    };
    // Years after a region empties before founders arrive from where it still lives.
    double earliest = 3.0;
    double latest = 10.0;
    double founders = 10.0; // animals in a founding group
    After after_extinction = After::Admin;
    // Years after it dies out everywhere before relicts or a successor turn up.
    double extinct_earliest = 20.0;
    double extinct_latest = 50.0;
    std::uint32_t successor = 0; // the species taking its place, for Successor
};

struct Species {
    std::string name;
    Moves moves = Moves::Land;
    double mass = 50.0;        // kg
    double appetite = 2.0;     // kg eaten per animal per day
    double half_fed = 1'000.0; // kg per km² of food on offer that gives an animal half its fill
    std::vector<Food> diet;    // in order of preference
    std::array<double, planet::biome_count> habitat{}; // how well it lives in each, 0 to 1
    double density = 1.0;  // animals per km² at the start, in the best habitat
    double crowded = 10.0; // animals per km² at which births stop, in the best habitat
    double births = 1.0;   // young per animal per year, well fed
    Breeding breeding{};
    double lifespan = 10.0; // years
    double starving = 0.05; // share dying per day with nothing to eat
    // Fed less than this share of its appetite, it starts to die of hunger; above it, hunger only
    // slows breeding.
    double hungry_below = 0.75;
    // °C of recent temperature it lives in comfortably.
    double cold = -30.0;
    double heat = 40.0;
    double exposure = 0.005; // share dying per day per °C outside that
    // Sleeps, neither eating nor breeding nor moving, while the recent temperature is below this.
    std::optional<double> dormant_below = std::nullopt;
    double wander = 0.2;    // km per day it strays at random
    double speed = 2.0;     // km per day it heads for better ground at
    double leftovers = 0.3; // share of each kill left for scavengers
    Cycle cycle{};
    Recovery recovery{};
};

struct Ecology {
    ForageConfig forage{};
    std::vector<Species> species;

    [[nodiscard]] std::optional<std::uint32_t> find(std::string_view name) const;
};

// Reads an ecology:
//
//     [forage]
//     growth_days = 30
//     wither_days = 90
//     cold = 0                     # °C
//     warm = 15
//     wet = 1.0                    # mm per day
//     rot_days = 10
//     capacity = { grassland = 300000, "temperate forest" = 200000 }    # kg per km²
//
//     [[species]]
//     name = "deer"
//     moves = "land"               # land, water or air
//     mass = 80
//     appetite = 3
//     half_fed = 20000
//     diet = { forage = 1.0 }      # forage, carrion or a species, with a preference
//     habitat = { grassland = 1.0, temperate_forest = 0.8 }
//     density = 2
//     crowded = 15
//     births = 0.9
//     breeding = { start_day = 40, days = 50 }
//     lifespan_years = 12
//     starving = 0.03
//     hungry_below = 0.75
//     comfort = [-25, 35]
//     exposure = 0.005
//     dormant_below = -10
//     wander_km = 0.2             # per day
//     speed_km = 2                # per day
//     leftovers = 0.3
//     cycle = { years = 13, boom = 4, width_years = 1, region_level = 2 }
//     recovery = { years = [3, 10], founders = 10, after_extinction = "successor",
//                  extinct_years = [20, 50], successor = "elk" }    # or "admin", "relict"
//
// Every key is optional except a species' name. Biomes are named as planet::to_string() names
// them, with spaces or underscores. Unknown keys, biomes and foods are errors.
[[nodiscard]] Result<Ecology> parse_ecology(std::string_view text);
[[nodiscard]] Result<Ecology> load_ecology(const std::filesystem::path& path);

} // namespace hog::sim
