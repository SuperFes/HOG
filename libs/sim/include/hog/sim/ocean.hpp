#pragma once

#include <hog/core/math.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/sim/weather_tuning.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace hog::jobs {
class JobSystem;
}

namespace hog::sim {

struct Ground;

// The sea under the weather, one value per grid cell. Vectors are (east, north); land has no
// current, and its temperature is the season's.
struct OceanFields {
    std::vector<float> temperature; // °C at the surface
    std::vector<Vec2> current;      // m/s at the surface
    std::vector<Vec2> stress;       // the wind's recent pull on the sea, N/m²
    std::vector<double> stream; // m²/s; currents run along its contours, faster where they crowd

    explicit OceanFields(std::size_t size = 0);

    [[nodiscard]] std::size_t size() const noexcept { return temperature.size(); }

    friend bool operator==(const OceanFields&, const OceanFields&) = default;
};

// Surface currents over the ocean cells of a weather grid, and the warmth they carry. Every call
// takes the grid it was made with.
class Ocean {
public:
    // A calm sea as warm as the season would make it with the sun at `sin_declination`.
    // `rotation` is the planet's spin in radians per second. Precondition: one ground per cell.
    Ocean(const CellGrid& grid, std::span<const Ground> ground, const OceanTuning& tuning,
          double rotation, double sin_declination);

    [[nodiscard]] const OceanFields& fields() const noexcept { return fields_; }
    // Replaces the sea as it stands, for loading and tests. Precondition: one value per cell.
    void set_fields(const CellGrid& grid, OceanFields fields);

    // Settles the currents at once under `wind`, as if it had long blown. Precondition: one wind
    // per cell.
    void settle_under(const CellGrid& grid, std::span<const Vec2> wind, jobs::JobSystem* jobs);
    // Moves the sea on by `seconds` under `wind`, with the sun at `sin_declination`.
    void step(const CellGrid& grid, std::span<const Vec2> wind, double sin_declination,
              double seconds, jobs::JobSystem* jobs);
    // The currents' part of a step: they follow the wind's recent pull.
    void drive(const CellGrid& grid, std::span<const Vec2> wind, double seconds,
               jobs::JobSystem* jobs);
    // The warmth's part: carried by the currents, cooled where colder water wells up, and pulled
    // towards the season.
    void warm(const CellGrid& grid, double sin_declination, double seconds, jobs::JobSystem* jobs);

private:
    // Where the sea would settle at the cell's latitude and season.
    [[nodiscard]] double target(const CellGrid& grid, std::uint32_t index,
                                double sin_declination) const;
    void settle(const CellGrid& grid, int sweeps, jobs::JobSystem* jobs);
    void find_currents(const CellGrid& grid, jobs::JobSystem* jobs);
    // How fast the wind's pull lifts water from below, in m/s: where it drives the surface apart,
    // off a coast or across the equator.
    void find_upwelling(const CellGrid& grid, jobs::JobSystem* jobs);

    OceanTuning tuning_;
    std::vector<bool> wet_;
    std::vector<float> mean_; // °C over a year
    bool coast_ = false;      // whether any land holds the stream in place
    // How the spin's growth towards the poles weighs each neighbour's stream: only those to the
    // east, where the planet's turning carries the flow's pattern from.
    std::vector<std::array<double, 4>> spin_weights_;
    double rotation_;
    OceanFields fields_;
    std::vector<double> forcing_; // the wind's twist, per second squared
    std::vector<double> next_;
    std::vector<double> upwelling_;
    std::vector<float> carried_;
};

} // namespace hog::sim
