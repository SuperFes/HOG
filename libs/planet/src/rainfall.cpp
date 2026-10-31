#include <hog/core/assert.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/rainfall.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace hog::planet {

namespace {

constexpr double wind_speed = 8.0;
// Of the wind's speed, the share it blows across the latitudes rather than along them.
constexpr double meridional = 0.4;
// Weather's eddies carry water every way at about this speed besides the prevailing wind, so
// it reaches the calms where the circulation's cells meet.
constexpr double eddies = 4.0;

// Millimetres a year evaporation could take at a mean temperature of 0 °C, and more a degree;
// roughly Thornthwaite's, never below the least the sun and wind take on cold ground.
constexpr std::int64_t evaporation_at_zero = 350;
constexpr std::int64_t evaporation_per_degree = 40;
constexpr std::int64_t least_evaporation = 100;

constexpr double seconds_per_year = 365.25 * 86'400.0;
// Sweeps after which rain still changing is left as it is.
constexpr int most_sweeps = 4'000;
constexpr double settled_vapour = 1e-4;

[[nodiscard]] DVec3 tangent(DVec3 vector, DVec3 at) {
    return vector - (at * glm::dot(vector, at));
}

// Millimetres of water saturated air holds over ground at `temperature` °C; Clausius-Clapeyron's
// rise without the exponential, so it's the same on every platform.
[[nodiscard]] double holding(double temperature, const Rainfall::Settings& settings) {
    const double base = std::max(0.0, 1.0 + (settings.holds_per_degree * temperature / 8.0));
    const double squared = base * base;
    const double fourth = squared * squared;
    return settings.holds * fourth * fourth;
}

// How strongly the circulation lifts air at the unit direction `at`: fully where its cells rise,
// at the equator and some 60 degrees out, least where they sink, at 30 degrees and the poles.
[[nodiscard]] double lift(DVec3 at, const Rainfall::Settings& settings) {
    const double u = at.z * at.z;
    // cos(6 * latitude) from sin²(latitude).
    const double bands = 1.0 - (18.0 * u) + (48.0 * u * u) - (32.0 * u * u * u);
    return settings.sinking_lift + ((1.0 - settings.sinking_lift) * (1.0 + bands) / 2.0);
}

// Where air reaching a cell comes from: its neighbours upwind, and around it as the eddies
// carry it.
struct Upwind {
    std::size_t first = 0;
    std::size_t count = 0;
    // Metres along the actual ground from where it comes.
    double reach = 0.0;
};

struct Air {
    std::vector<Upwind> upwind;
    std::vector<std::uint32_t> from;
    std::vector<double> share;
};

[[nodiscard]] Air upwind_of(const LevelGrid& grid, std::span<const DVec3> centers) {
    Air air;
    air.upwind.resize(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const DVec3 here = centers[index];
        const DVec3 wind = prevailing_wind(here);
        const double speed = glm::length(wind);
        const DVec3 way = speed > 0.0 ? wind / speed : DVec3{0.0};
        const auto around = grid.neighbors(index);
        const std::size_t first = air.from.size();
        double upwind = 0.0;
        for (const std::uint32_t neighbor : around) {
            const DVec3 to = tangent(centers[neighbor] - here, here);
            const double facing = std::max(0.0, -glm::dot(to / glm::length(to), way));
            air.from.push_back(neighbor);
            air.share.push_back(facing * facing);
            upwind += facing * facing;
        }
        const double windy = (speed * speed) / ((speed * speed) + (eddies * eddies));
        const auto count = static_cast<double>(around.size());
        double reach = 0.0;
        for (std::size_t k = 0; k < around.size(); ++k) {
            double& share = air.share[first + k];
            share = ((1.0 - windy) / count) + (upwind > 0.0 ? windy * share / upwind : 0.0);
            reach += share * fixed_to_double(grid.distance(index, k));
        }
        air.upwind[index] = {.first = first, .count = around.size(), .reach = reach};
    }
    return air;
}

// What doesn't change about a cell as the water moves.
struct Cell {
    bool sea = false;
    // Millimetres of water the air holds saturated here, and where it comes from.
    double holds = 0.0;
    double upwind_holds = 0.0;
    // Seconds the air takes to arrive from upwind.
    double travel = 0.0;
    double lift = 0.0;
    // Of the cloud the air carries, the share that falls here.
    double falls = 0.0;
};

// Millimetres of vapour and of cloud the air carries on from each cell, and millimetres a second
// of rain there.
struct Water {
    std::vector<double> vapour;
    std::vector<double> cloud;
    std::vector<double> rain;
};

// Carries the water a cell on with the wind, in place, so what's carried on reaches the cells
// downwind in the same sweep; returns the most any cell's vapour changed.
[[nodiscard]] double carry(const Air& air, std::span<const Cell> cells,
                           const Rainfall::Settings& settings, Water& water) {
    double change = 0.0;
    for (std::uint32_t index = 0; index < cells.size(); ++index) {
        const Cell& cell = cells[index];
        const Upwind& upwind = air.upwind[index];
        double carried = 0.0;
        double clouded = 0.0;
        for (std::size_t k = 0; k < upwind.count; ++k) {
            const double share = air.share[upwind.first + k];
            const std::uint32_t from = air.from[upwind.first + k];
            carried += share * water.vapour[from];
            clouded += share * water.cloud[from];
        }
        // Taken up and rained out on the way, implicitly so any step is stable. The sea only
        // gives water to air drier than it brings it to, never takes it back.
        const double raining = cell.lift / settings.raining;
        const double recycled = cell.sea ? 0.0 : settings.recycled * water.rain[index];
        double held = (carried + (cell.travel * recycled)) / (1.0 + (cell.travel * raining));
        if (cell.sea) {
            const double evaporating = 1.0 / settings.evaporating;
            const double humid = settings.sea_humidity * cell.holds;
            held = std::max(held, (carried + (cell.travel * humid * evaporating)) /
                                      (1.0 + (cell.travel * (raining + evaporating))));
        }
        // Air coming down warms and takes its cloud up again; air climbing cools and can't hold
        // all it carries.
        const double warmed = std::min(clouded, std::max(0.0, cell.holds - cell.upwind_holds));
        held += warmed;
        clouded -= warmed;
        if (held > cell.holds) {
            clouded += held - cell.holds;
            held = cell.holds;
        }
        const double fallen = clouded * cell.falls;
        change = std::max(change, std::abs(held - water.vapour[index]));
        water.vapour[index] = held;
        water.cloud[index] = clouded - fallen;
        water.rain[index] = (raining * held) + (fallen / std::max(cell.travel, 1.0));
    }
    return change;
}

} // namespace

DVec3 prevailing_wind(DVec3 at) {
    const double across = std::sqrt(std::max(0.0, 1.0 - (at.z * at.z)));
    if (across < 1e-9) {
        return DVec3{0.0};
    }
    const double sine = std::abs(at.z);
    const double sine3 = (3.0 * sine) - (4.0 * sine * sine * sine);
    const double cosine3 = (4.0 * across * across * across) - (3.0 * across);
    // sin(6 * latitude): the trades' cell, the westerlies' and the polar easterlies' by sign.
    // Air turns to rise or sink where the cells meet, so it blows across the latitudes as they
    // have it; the trades keep blowing from the east across the equator, as cos(3 * latitude).
    const double cells = 2.0 * sine3 * cosine3;
    const double easterly = sine < 0.5 ? cosine3 : cells;
    const DVec3 east = DVec3{-at.y, at.x, 0.0} / across;
    const DVec3 poleward = glm::cross(at, east) * (at.z < 0.0 ? -1.0 : 1.0);
    return -wind_speed * ((easterly * east) + (meridional * cells * poleward));
}

std::int64_t wetness(std::int64_t rain, std::int64_t temperature) {
    const std::int64_t evaporation =
        std::max(least_evaporation * fixed_one,
                 (evaporation_at_zero * fixed_one) + (evaporation_per_degree * temperature));
    const std::int64_t wet = std::max(rain, std::int64_t{0});
    return (wet * fixed_one) / (wet + evaporation);
}

Rainfall::Rainfall(LevelGrid grid, std::span<const std::int64_t> ground,
                   std::span<const std::int64_t> warmth, double radius, Settings settings)
    : grid_{std::move(grid)}, rain_(grid_.size()) {
    HOG_ASSERT(ground.size() == grid_.size() && warmth.size() == grid_.size(),
               "ground and warmth for every cell");
    const std::uint32_t size = grid_.size();
    std::vector<DVec3> centers(size);
    for (std::uint32_t index = 0; index < size; ++index) {
        centers[index] = cell_center(grid_.cell(index));
    }
    const Air air = upwind_of(grid_, centers);
    const double scale = settings.earth_radius / radius;

    std::vector<Cell> cells(size);
    for (std::uint32_t index = 0; index < size; ++index) {
        Cell& cell = cells[index];
        cell.sea = ground[index] < 0;
        cell.holds = holding(fixed_to_double(warmth[index]), settings);
        cell.lift = lift(centers[index], settings);
        const double wind = glm::length(prevailing_wind(centers[index]));
        const double speed = std::sqrt((wind * wind) + (eddies * eddies));
        cell.travel = air.upwind[index].reach * scale / speed;
        cell.falls = std::min(1.0, air.upwind[index].reach / settings.drift);
    }
    for (std::uint32_t index = 0; index < size; ++index) {
        const Upwind& upwind = air.upwind[index];
        for (std::size_t k = 0; k < upwind.count; ++k) {
            cells[index].upwind_holds +=
                air.share[upwind.first + k] * cells[air.from[upwind.first + k]].holds;
        }
    }
    Water water{.vapour = std::vector<double>(size),
                .cloud = std::vector<double>(size, 0.0),
                .rain = std::vector<double>(size, 0.0)};
    for (std::uint32_t index = 0; index < size; ++index) {
        water.vapour[index] = settings.sea_humidity * cells[index].holds;
    }
    for (int sweep = 0; sweep < most_sweeps; ++sweep) {
        if (carry(air, cells, settings, water) < settled_vapour) {
            break;
        }
    }
    for (std::uint32_t index = 0; index < size; ++index) {
        rain_[index] =
            std::llround(water.rain[index] * seconds_per_year * static_cast<double>(fixed_one));
    }
}

std::int64_t Rainfall::at(CellId cell) const {
    return std::max(grid_.smooth(rain_, cell), std::int64_t{0});
}

} // namespace hog::planet
