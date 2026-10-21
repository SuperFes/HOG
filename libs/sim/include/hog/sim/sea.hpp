#pragma once

#include <hog/core/math.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/sim/weather_tuning.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::jobs {
class JobSystem;
}

namespace hog::sim {

struct Ground;

// Swell is kept in the waves' longest bands, along this many headings evenly round from east
// towards north: shorter swell wears away within a cell or two of where it's left.
inline constexpr int swell_bands = 8;
inline constexpr int swell_headings = 12;
inline constexpr std::size_t swell_bins = std::size_t{swell_bands} * swell_headings;

// The sea's waves, one value per grid cell, as their variance in m²: a sixteenth of the square of
// the height they make. Headings are (east, north), the way the waves run.
struct SeaFields {
    std::vector<float> wind_sea;
    std::vector<float> peak;   // rad/s, the wind sea's; 0 where there's none
    std::vector<Vec2> heading; // unit, the wind sea's
    // Cell by cell, band by band, heading by heading.
    std::vector<float> swell;

    explicit SeaFields(std::size_t size = 0);

    [[nodiscard]] std::size_t size() const noexcept { return wind_sea.size(); }
    [[nodiscard]] std::span<const float> swell_at(std::size_t index) const;

    friend bool operator==(const SeaFields&, const SeaFields&) = default;
};

// A system of waves over a cell.
struct WaveSystem {
    float height = 0.0F; // significant, metres
    float peak = 0.0F;   // rad/s
    Vec2 heading{1.0F, 0.0F};

    friend bool operator==(const WaveSystem&, const WaveSystem&) = default;
};

// The two strongest swells among one cell's, strongest first, each the variance of a few
// neighbouring headings; none where there's none. What's left is too little or too spread to
// matter.
[[nodiscard]] std::array<WaveSystem, 2> strongest_swells(std::span<const float> swell);

// The waves over the ocean cells of a weather grid. Every call takes the grid it was made with.
class Sea {
public:
    // A calm sea. Precondition: one ground per cell.
    Sea(const CellGrid& grid, std::span<const Ground> ground, const SeaTuning& tuning);

    [[nodiscard]] const SeaFields& fields() const noexcept { return fields_; }
    // Replaces the sea as it stands, for loading and tests. Precondition: one value per cell.
    void set_fields(const CellGrid& grid, SeaFields fields);

    // Calms the sea, then raises its wind sea as `seconds` of `wind` would have.
    void raise(const CellGrid& grid, std::span<const Vec2> wind, double seconds);
    // Moves the sea on by `seconds` under `wind`. Precondition: one wind per cell.
    void step(const CellGrid& grid, std::span<const Vec2> wind, double seconds,
              jobs::JobSystem* jobs);

    [[nodiscard]] WaveSystem wind_sea(std::uint32_t index) const;

private:
    void run_swell(const CellGrid& grid, double seconds, jobs::JobSystem* jobs);
    void run_wind_sea(const CellGrid& grid, std::span<const Vec2> wind, double seconds,
                      jobs::JobSystem* jobs);

    SeaTuning tuning_;
    std::vector<bool> wet_;
    // How far each neighbour's headings are turned from this cell's, in headings.
    std::vector<std::array<double, 4>> turns_;
    // The longest step that carries the fastest swell less than half a cell.
    double longest_step_ = 0.0;
    SeaFields fields_;
    SeaFields before_;
};

} // namespace hog::sim
