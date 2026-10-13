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

// The sea under the weather, one value per grid cell; land cells hold none. Vectors are (east,
// north).
struct OceanFields {
    std::vector<Vec2> current;  // m/s at the surface
    std::vector<Vec2> stress;   // the wind's recent pull on the sea, N/m²
    std::vector<double> stream; // m²/s; currents run along its contours, faster where they crowd

    explicit OceanFields(std::size_t size = 0);

    [[nodiscard]] std::size_t size() const noexcept { return stream.size(); }

    friend bool operator==(const OceanFields&, const OceanFields&) = default;
};

// Surface currents over the ocean cells of a weather grid. Every call takes the grid and ground it
// was made with.
class Ocean {
public:
    // Settles the currents under `wind`, from rest. `rotation` is the planet's spin in radians per
    // second. Precondition: one ground and one wind per cell.
    Ocean(const CellGrid& grid, std::span<const Ground> ground, const OceanTuning& tuning,
          double rotation, std::span<const Vec2> wind);

    [[nodiscard]] const OceanFields& fields() const noexcept { return fields_; }
    // Replaces the sea as it stands, for loading and tests. Precondition: one value per cell.
    void set_fields(const CellGrid& grid, OceanFields fields);

    // Moves the sea on by `seconds` under `wind`.
    void step(const CellGrid& grid, std::span<const Vec2> wind, double seconds,
              jobs::JobSystem* jobs);

private:
    void settle(const CellGrid& grid, int sweeps, jobs::JobSystem* jobs);
    void find_currents(const CellGrid& grid, jobs::JobSystem* jobs);

    OceanTuning tuning_;
    std::vector<bool> wet_;
    bool coast_ = false; // whether any land holds the stream in place
    // How the spin's growth towards the poles weighs each neighbour's stream: only those to the
    // east, where the planet's turning carries the flow's pattern from.
    std::vector<std::array<double, 4>> spin_weights_;
    OceanFields fields_;
    std::vector<double> forcing_; // the wind's twist, per second squared
    std::vector<double> next_;
};

} // namespace hog::sim
