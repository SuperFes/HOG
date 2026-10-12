#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/sim/vegetation.hpp>

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace hog::jobs {
class JobSystem;
}

namespace hog::sim {

class Weather;

struct ForestConfig {
    int level = 5;
    // Steps fall on whole multiples of this since the epoch.
    GameDuration step = std::chrono::hours{1};
    double planet_radius = 115'000.0; // metres, for the cells' areas
    std::uint64_t seed = 0;
    Vegetation vegetation{};
};

// What a cell's plants grow on.
struct Soil {
    planet::Biome biome = planet::Biome::Grassland;
    float moisture = 0.5F; // from the planet's moisture map, 0 (arid) to 1 (saturated)
};

[[nodiscard]] std::vector<Soil> soil_for(const CellGrid& grid, const planet::World& world);

// The weather in each cell, as plants feel it.
struct ForestConditions {
    std::span<const float> temperature;   // recent °C
    std::span<const float> precipitation; // recent mm per day
};

// Precondition: the weather's grid has the forest's level.
[[nodiscard]] ForestConditions forest_conditions_of(const Weather& weather);

struct ForestFields {
    std::vector<float> biomass; // kg per m² of living plants
    std::vector<float> dryness; // of the ground at the last step, 0 (wet) to 1 (parched)

    friend bool operator==(const ForestFields&, const ForestFields&) = default;
};

// How far a cell's plants have grown back, by their share of what its biome holds.
enum class Stage : std::uint8_t { Bare, Grass, Shrub, Young, Mature };

[[nodiscard]] Stage stage_of(double biomass, double capacity) noexcept;
[[nodiscard]] std::string_view to_string(Stage stage) noexcept;

// How dry the ground is, from 0 (wet) to 1 (parched): the moisture map blended with how well the
// recent rain keeps up with what the warmth draws out. Stands in for soil moisture.
[[nodiscard]] double dryness(const DrynessConfig& config, double moisture, double temperature,
                             double precipitation) noexcept;

// Living plants per cell, as one amount of biomass growing back towards what its biome holds, as
// fast as the warmth and the wetness of the ground allow.
class Forest {
public:
    // Fully grown, as of the last step at or before `now`. Precondition: soil for every cell at
    // the configured level, a positive step.
    Forest(const ForestConfig& config, std::vector<Soil> soil, GameTime now);

    [[nodiscard]] const ForestConfig& config() const noexcept { return config_; }
    [[nodiscard]] const CellGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] const std::vector<Soil>& soil() const noexcept { return soil_; }
    [[nodiscard]] const ForestFields& fields() const noexcept { return fields_; }
    // km².
    [[nodiscard]] double area(std::uint32_t cell) const { return areas_[cell]; }
    // kg per m² the cell's biome holds fully grown.
    [[nodiscard]] double capacity(std::uint32_t cell) const;
    [[nodiscard]] Stage stage(std::uint32_t cell) const;
    // When the last step was taken, and when the next is due.
    [[nodiscard]] GameTime time() const noexcept { return time_; }
    [[nodiscard]] GameTime next_step() const noexcept { return time_ + config_.step; }

    // Takes every step due by `now`, all under the same conditions; returns how many. Call from
    // the jobs' owning thread. Precondition: conditions for every cell.
    std::uint64_t advance_to(GameTime now, const ForestConditions& conditions,
                             jobs::JobSystem& jobs);

    // kg of living plants across the planet.
    [[nodiscard]] double total() const;

private:
    void step(GameTime at, const ForestConditions& conditions, jobs::JobSystem& jobs);

    ForestConfig config_;
    CellGrid grid_;
    std::vector<Soil> soil_;
    std::vector<double> areas_;
    ForestFields fields_;
    GameTime time_;
};

} // namespace hog::sim
