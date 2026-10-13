#pragma once

#include <hog/sim/cell_grid.hpp>
#include <hog/sim/weather_tuning.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::jobs {
class JobSystem;
}

namespace hog::sim {

struct Ground;

// The water each land cell holds, one value per grid cell; the sea's cells hold none.
struct LandFields {
    std::vector<float> snow;         // mm of water lying as snow
    std::vector<float> soil;         // how full the soil is, 0 to 1
    std::vector<double> groundwater; // mm stored below the soil
    // What the last step sent towards the rivers, in mm: run off the surface and seeped out of
    // the ground.
    std::vector<float> runoff;
    std::vector<double> usual_groundwater; // its running mean, mm
    std::vector<double> channel;           // m³ of water in the cell's rivers
    std::vector<float> flow;               // m³/s leaving the cell down its rivers
    std::vector<double> usual_flow;        // its running mean, m³/s

    explicit LandFields(std::size_t size = 0);

    [[nodiscard]] std::size_t size() const noexcept { return snow.size(); }

    friend bool operator==(const LandFields&, const LandFields&) = default;
};

// What the weather did to the ground over a step, per cell.
struct LandWeather {
    std::span<const float> rain;        // mm
    std::span<const float> snow;        // mm of water
    std::span<const float> hail;        // mm of water
    std::span<const float> evaporation; // mm drawn from the surface
    std::span<const float> temperature; // °C of the surface air
    double seconds = 0.0;               // the step's length
};

// Snow lies until the warmth melts it. Rain and meltwater soak into the soil, less readily the
// fuller it is, and what doesn't soak in runs off. The soil drains slowly into the ground water,
// which seeps out to the rivers over seasons, so wells and springs rise and fall with them. Rivers
// carry what runs off and seeps out from cell to cell down the ground to the sea, rising in floods
// after rain and thaw and falling low through dry spells. Every call takes the grid it was made
// with.
class Land {
public:
    // Bare and dry, with rivers running down the ground's elevation. Precondition: one ground per
    // cell, a positive radius in metres.
    Land(const CellGrid& grid, std::span<const Ground> ground, const LandTuning& tuning,
         double planet_radius);

    [[nodiscard]] const LandFields& fields() const noexcept { return fields_; }
    // Replaces the water as it stands, for loading and tests. Precondition: one value per cell.
    void set_fields(const CellGrid& grid, LandFields fields);

    // Share of the ground snow hides, 0 to 1.
    [[nodiscard]] double snow_cover(std::uint32_t index) const;
    // How freely the ground gives up water, 0 to 1: as wet as its soil, less under snow.
    [[nodiscard]] double wetness(std::uint32_t index) const;
    // Metres the water table stands above its usual level, or below where negative.
    [[nodiscard]] double water_table_rise(std::uint32_t index) const;
    // How the cell's river flows against its usual, 1 for usual: higher in floods, lower in
    // droughts.
    [[nodiscard]] double flow_ratio(std::uint32_t index) const;
    // mm of water the cell holds as snow, in its soil and in the ground.
    [[nodiscard]] double water(std::uint32_t index) const;
    // The cell its rivers run into: downhill, or the sea; a cell with nowhere lower to go, its
    // own.
    [[nodiscard]] std::uint32_t receiver(std::uint32_t index) const { return receiver_[index]; }
    // m³ the rivers have carried into the sea, or into basins with no way out, since the land was
    // made or loaded.
    [[nodiscard]] double discharged() const noexcept { return discharged_; }

    // Moves the water on by a step of the weather. Precondition: weather for every cell.
    void step(const CellGrid& grid, const LandWeather& weather, jobs::JobSystem* jobs);

private:
    void step(std::uint32_t index, const LandWeather& weather);
    void route(double seconds);

    LandTuning tuning_;
    std::vector<bool> land_;
    std::vector<double> areas_; // m²
    std::vector<std::uint32_t> receiver_;
    // Every cell after those upstream of it.
    std::vector<std::uint32_t> downstream_;
    LandFields fields_;
    double discharged_ = 0.0;
};

} // namespace hog::sim
