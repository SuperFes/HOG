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

    explicit LandFields(std::size_t size = 0);

    [[nodiscard]] std::size_t size() const noexcept { return snow.size(); }

    friend bool operator==(const LandFields&, const LandFields&) = default;
};

// What the weather did to the ground over a step, per cell.
struct LandWeather {
    std::span<const float> rain;        // mm
    std::span<const float> snow;        // mm of water, hail included
    std::span<const float> evaporation; // mm drawn from the surface
    std::span<const float> temperature; // °C of the surface air
    double seconds = 0.0;               // the step's length
};

// Snow lies until the warmth melts it. Rain and meltwater soak into the soil, less readily the
// fuller it is, and what doesn't soak in runs off. The soil drains slowly into the ground water,
// which seeps out to the rivers over seasons, so wells and springs rise and fall with them. Every
// call takes the grid it was made with.
class Land {
public:
    // Bare and dry. Precondition: one ground per cell.
    Land(const CellGrid& grid, std::span<const Ground> ground, const LandTuning& tuning);

    [[nodiscard]] const LandFields& fields() const noexcept { return fields_; }
    // Replaces the water as it stands, for loading and tests. Precondition: one value per cell.
    void set_fields(const CellGrid& grid, LandFields fields);

    // Share of the ground snow hides, 0 to 1.
    [[nodiscard]] double snow_cover(std::uint32_t index) const;
    // How freely the ground gives up water, 0 to 1: as wet as its soil, less under snow.
    [[nodiscard]] double wetness(std::uint32_t index) const;
    // Metres the water table stands above its usual level, or below where negative.
    [[nodiscard]] double water_table_rise(std::uint32_t index) const;
    // mm of water the cell holds as snow, in its soil and in the ground.
    [[nodiscard]] double water(std::uint32_t index) const;

    // Moves the water on by a step of the weather. Precondition: weather for every cell.
    void step(const CellGrid& grid, const LandWeather& weather, jobs::JobSystem* jobs);

private:
    void step(std::uint32_t index, const LandWeather& weather);

    LandTuning tuning_;
    std::vector<bool> land_;
    LandFields fields_;
};

} // namespace hog::sim
