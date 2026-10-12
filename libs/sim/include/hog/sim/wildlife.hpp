#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/sim/ecology.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::jobs {
class JobSystem;
}

namespace hog::sim {

class Weather;

namespace detail {
struct LifeContext;
}

struct WildlifeConfig {
    int level = 5;
    // Steps fall on whole multiples of this since the epoch.
    GameDuration step = std::chrono::hours{24};
    double planet_radius = 115'000.0; // metres, for the cells' areas
    std::uint64_t seed = 0;           // where authored cycles peak first
    double most_eaten = 0.5;          // share of any food that can be eaten in a day
    double extinct_below = 1e-3;      // animals; a cell with fewer has none
    // Animals sense better ground this many cells away, valued less by `scent_fade` per cell.
    int scent_reach = 12;
    double scent_fade = 0.85;
    Ecology ecology{};
};

// What a cell offers wildlife.
struct Habitat {
    planet::Biome biome = planet::Biome::Grassland;
    bool ocean = false;
};

[[nodiscard]] std::vector<Habitat> habitat_for(const CellGrid& grid, const planet::World& world);

// The recent weather in each cell.
struct Conditions {
    std::span<const float> temperature;   // °C
    std::span<const float> precipitation; // mm per day
};

// Precondition: the weather's grid has the wildlife's level.
[[nodiscard]] Conditions conditions_of(const Weather& weather);

struct Population {
    std::vector<float> count; // animals in each cell
    std::vector<float> fed;   // share of their appetite met on the last step, 0 to 1

    friend bool operator==(const Population&, const Population&) = default;
};

struct WildlifeFields {
    std::vector<float> forage;           // kg per km²
    std::vector<float> carrion;          // kg per km²
    std::vector<Population> populations; // one per species, in the ecology's order

    friend bool operator==(const WildlifeFields&, const WildlifeFields&) = default;
};

// Populations of each species per cell, stepped together: plants grow, animals eat, are born and
// die, and wander or head for better ground. Long cycles come from predators and prey chasing
// each other and from good and bad years of weather, plus any a species is authored with.
class Wildlife {
public:
    // Starts at the ecology's densities with forage grown in, as of the last step at or before
    // `now`. Precondition: one habitat per cell at the configured level, a positive step.
    Wildlife(WildlifeConfig config, std::vector<Habitat> habitat, GameTime now);

    [[nodiscard]] const WildlifeConfig& config() const noexcept { return config_; }
    [[nodiscard]] const CellGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] const std::vector<Habitat>& habitat() const noexcept { return habitat_; }
    [[nodiscard]] const WildlifeFields& fields() const noexcept { return fields_; }
    // km².
    [[nodiscard]] double area(std::uint32_t cell) const { return areas_[cell]; }
    // When the last step was taken, and when the next is due.
    [[nodiscard]] GameTime time() const noexcept { return time_; }
    [[nodiscard]] GameTime next_step() const noexcept { return time_ + config_.step; }

    // Takes every step due by `now`, all under the same conditions; returns how many. Call from
    // the jobs' owning thread. Precondition: conditions for every cell.
    std::uint64_t advance_to(GameTime now, const Conditions& conditions, jobs::JobSystem& jobs);

    // Animals of a species across the planet.
    [[nodiscard]] double total(std::uint32_t species) const;
    // Removes up to `count` animals from a cell, as when they're killed or brought to life in
    // detail; returns how many there were to take.
    float take(std::uint32_t species, std::uint32_t cell, float count);
    // Puts animals back, as when they're folded back into the population.
    void add(std::uint32_t species, std::uint32_t cell, float count);

    // Versioned bytes for a snapshot section.
    [[nodiscard]] std::vector<std::byte> save() const;
    // Replaces the state with saved bytes, or fails and changes nothing: with Unsupported for a
    // newer version, and InvalidArgument for wildlife saved at another grid level. Species are
    // matched by name; one the save lacks keeps its state.
    [[nodiscard]] Result<void> load(std::uint16_t version, std::span<const std::byte> bytes);

    static constexpr std::uint16_t save_version = 1;

private:
    void step(GameTime at, const Conditions& conditions, jobs::JobSystem& jobs);
    // Animals wander, and head for better ground they can sense.
    void move(const detail::LifeContext& context, std::uint32_t species, jobs::JobSystem& jobs);

    WildlifeConfig config_;
    CellGrid grid_;
    std::vector<Habitat> habitat_;
    std::vector<double> areas_;
    // For each neighbour of a cell, which of its neighbours is the cell.
    std::vector<std::array<std::uint8_t, 4>> back_;
    // Per species, the year (from 0 to its cycle's length) each cell's authored boom peaks.
    std::vector<std::vector<float>> peaks_;
    WildlifeFields fields_;
    GameTime time_;
    // Scratch for moving.
    std::vector<float> appeal_;
    std::vector<float> scent_;
    std::vector<float> scent_next_;
    std::vector<std::array<float, 4>> leaving_;
    std::vector<float> arriving_;
};

} // namespace hog::sim
