#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/sim/vegetation.hpp>

#include <chrono>
#include <cstddef>
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
    std::uint64_t seed = 0;           // where and when lightning starts fires
    Vegetation vegetation{};
};

// What a cell's plants grow on.
struct Soil {
    planet::Biome biome = planet::Biome::Grassland;
};

[[nodiscard]] std::vector<Soil> soil_for(const CellGrid& grid, const planet::World& world);

// The weather in each cell, as plants and fires feel it.
struct ForestConditions {
    std::span<const float> temperature; // recent °C
    std::span<const float> soil;        // how full the soil is, 0 to 1
    std::span<const float> snow;        // mm of water lying as snow
    std::span<const float> rain;        // mm fallen over the last `rain_days`
    double rain_days = 1.0;
    std::span<const float> storm; // thunderstorm strength now, 0 to 1
    std::span<const Vec2> wind;   // (east, north) m/s at the surface
    // mm stored below the soil, and its running mean; both empty where it isn't known, and then
    // nothing dies of drought.
    std::span<const double> groundwater;
    std::span<const double> usual_groundwater;
};

// Precondition: the weather's grid has the forest's level.
[[nodiscard]] ForestConditions forest_conditions_of(const Weather& weather);

struct ForestFields {
    std::vector<float> biomass; // kg per m² of living plants
    std::vector<float> dryness; // of the ground at the last step, 0 (wet) to 1 (parched)
    std::vector<float> fire;    // share of the cell burning, 0 to 1
    // The fates of what each cell holds, each a share of it as the living share (biomass over
    // capacity) is: the living since before their last loss, at most all the living; the living
    // dying back; the dead still standing; and of those, the share burned.
    std::vector<float> established;
    std::vector<float> failing;
    std::vector<float> dead;
    std::vector<float> charred;
    // How far the ground water has fallen below its usual level, as a share of it, felt as a
    // running mean, and the worst of it the plants are used to.
    std::vector<float> shortfall;
    std::vector<float> accustomed;
    // Of the cell's ground, the share a fire left black, greening again over the years.
    std::vector<float> scar;

    friend bool operator==(const ForestFields&, const ForestFields&) = default;
};

// Fires since last asked.
struct FireReport {
    std::uint64_t started = 0; // by lightning or by hand
    double alight = 0.0;       // km² of cells that caught fire, however they caught it

    friend bool operator==(const FireReport&, const FireReport&) = default;
};

// How far a cell's plants have grown back, by their share of what its biome holds.
enum class Stage : std::uint8_t { Bare, Grass, Shrub, Young, Mature };

[[nodiscard]] Stage stage_of(double biomass, double capacity) noexcept;
[[nodiscard]] std::string_view to_string(Stage stage) noexcept;

// How dry the ground is, from 0 (wet) to 1 (parched): as dry as its soil is empty, and not at
// all under snow.
[[nodiscard]] double dryness(double soil, double snow) noexcept;

// Living plants per cell, as one amount of biomass growing back towards what its biome holds, as
// fast as the warmth and the wetness of the ground allow. Lightning sets dry fuel alight, and
// fire spreads to neighbours that will burn, most readily downwind; it burns its cell's plants
// down to the seeds they grow back from, and goes out as the fuel runs low or the rain puts it out.
// Where the ground water stays further below its usual level than in the dry seasons the plants
// are used to, they die back to what it can keep. What dies stands dead, burned or not, until it
// falls; what grows back takes generations to be old growth again.
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
    // Of what the cell holds, the share living: 1 where it holds nothing.
    [[nodiscard]] double living(std::uint32_t cell) const;
    // Of what the cell holds, the share the ground water can keep alive now.
    [[nodiscard]] double sustainable(std::uint32_t cell) const;
    // How readily the cell burns as it stands, 0 to 1, from its dryness and fuel.
    [[nodiscard]] double flammability(std::uint32_t cell) const;
    // When the last step was taken, and when the next is due.
    [[nodiscard]] GameTime time() const noexcept { return time_; }
    [[nodiscard]] GameTime next_step() const noexcept { return time_ + config_.step; }
    // Until then, the plants are still learning what dry seasons are usual, and none die of them.
    [[nodiscard]] GameTime learning_until() const noexcept { return learning_until_; }

    // Takes every step due by `now`, all under the same conditions; returns how many. Call from
    // the jobs' owning thread. Precondition: conditions for every cell.
    std::uint64_t advance_to(GameTime now, const ForestConditions& conditions,
                             jobs::JobSystem& jobs);

    // kg of living plants across the planet.
    [[nodiscard]] double total() const;
    // Cells alight.
    [[nodiscard]] std::uint32_t burning() const;
    // Sets a cell alight, as someone starting a fire does; fails where too little would burn.
    bool ignite(std::uint32_t cell);
    // Leaves a cell as a fire `years` ago would have, burned to the seeds and grown back since
    // under a climate of `temperature` °C and `dryness` year round: for a new world's history.
    void burned_ago(std::uint32_t cell, double years, double temperature, double dryness);
    // Fires since the last call. They aren't saved.
    [[nodiscard]] FireReport drain_fires();

    // Versioned bytes for a snapshot section.
    [[nodiscard]] std::vector<std::byte> save() const;
    // Replaces the state with saved bytes, or fails and changes nothing: with Unsupported for a
    // newer version, and InvalidArgument for a forest saved at another grid level.
    [[nodiscard]] Result<void> load(std::uint16_t version, std::span<const std::byte> bytes);

    static constexpr std::uint16_t save_version = 3;

private:
    // How a cell catches fire on a step.
    enum class Catching : std::uint8_t { No, Spread, Lightning };

    void step(GameTime at, const ForestConditions& conditions, jobs::JobSystem& jobs);
    [[nodiscard]] double flammability(std::uint32_t cell, double biomass) const;
    // Whether the cell catches fire from lightning or a burning neighbour this step.
    [[nodiscard]] Catching catching(std::uint32_t cell, const ForestConditions& conditions,
                                    double days, std::uint64_t step_number) const;
    // Burns or grows the cell's plants over the step.
    void tend(std::uint32_t cell, const ForestConditions& conditions, double days);
    // Feels the cell's ground water against its usual level.
    void feel(std::uint32_t cell, const ForestConditions& conditions, double days);
    // Keeps the cell's fates as its living share went from `before` to `after` over the step,
    // what was lost burned or not.
    void settle(std::uint32_t cell, double before, double after, bool burned, double days);

    ForestConfig config_;
    CellGrid grid_;
    std::vector<Soil> soil_;
    std::vector<double> areas_;
    ForestFields fields_;
    FireReport fires_;
    GameTime time_;
    GameTime learning_until_;
    // Scratch for stepping.
    std::vector<Catching> catching_;
};

} // namespace hog::sim
