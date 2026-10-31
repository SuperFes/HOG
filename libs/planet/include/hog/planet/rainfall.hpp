#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/level_grid.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// Metres a second of the wind at the surface under the unit direction `at`, along the ground:
// easterly trades blowing towards the equator, westerlies blowing towards the poles and polar
// easterlies, calm where the circulation's cells meet outside the tropics.
[[nodiscard]] DVec3 prevailing_wind(DVec3 at);

// Fixed point from 0 (arid) to 1: the share rain makes of itself and of what the warmth could
// evaporate, P / (P + PET), given fixed-point millimetres of rain a year and °C.
[[nodiscard]] std::int64_t wetness(std::int64_t rain, std::int64_t temperature);

// The rain the winds bring. They take water up off the sea and lose it as rain where the air
// rises: in the bands where the circulation lifts it, and where it climbs the ground and cools
// until it can't hold what it carries. Coming down the far side, air left dry is the range's
// rain shadow, and far inland, with only what the ground gives back, it dries out. Solved as it
// stands once the winds have carried water for long enough.
class Rainfall {
public:
    // Times in seconds; lengths along the ground in metres, as if on a planet Earth's size, so
    // continents dry out inland as Earth's do on a planet of any size.
    struct Settings {
        // Millimetres of water saturated air holds over ground at 0 °C, rising by this share a
        // degree.
        double holds = 10.0;
        double holds_per_degree = 0.066;
        // Air over the sea is brought to this share of saturated over `evaporating`.
        double sea_humidity = 0.8;
        double evaporating = 2.0 * 86'400.0;
        // Where the circulation lifts air fully, it rains out what it carries over `raining`;
        // between its cells, where air sinks, this little.
        double raining = 4.5 * 86'400.0;
        double sinking_lift = 0.1;
        // Of the rain on land, the share plants and soil give back to the air.
        double recycled = 0.4;
        // Metres along the actual ground rain and snow drift on the wind as they fall.
        double drift = 4'000.0;
        double earth_radius = 6'371'000.0;
    };

    // `ground` holds fixed-point metres above the sea at each of `grid`'s cells, negative where
    // the sea covers them, and `warmth` fixed-point °C at the ground there, or at the sea's
    // surface; `radius` is the planet's, in metres.
    Rainfall(LevelGrid grid, std::span<const std::int64_t> ground,
             std::span<const std::int64_t> warmth, double radius, Settings settings);

    [[nodiscard]] const LevelGrid& grid() const noexcept { return grid_; }
    // Fixed-point millimetres a year at each of the grid's cells.
    [[nodiscard]] std::span<const std::int64_t> rain() const noexcept { return rain_; }
    // The same at any cell's centre, smooth between the grid's.
    [[nodiscard]] std::int64_t at(CellId cell) const;

private:
    LevelGrid grid_;
    std::vector<std::int64_t> rain_;
};

} // namespace hog::planet
