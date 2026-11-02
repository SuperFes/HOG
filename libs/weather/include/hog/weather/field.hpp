#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/generator.hpp>
#include <hog/weather/grid.hpp>

#include <cstdint>
#include <vector>

namespace hog::weather {

// What one cell of air holds. Water is per kilogram of dry air; where the planet has another
// volatile, that is what it means.
struct Air {
    float temperature = 0.0F; // kelvin
    float vapour = 0.0F;
    float cloud = 0.0F; // liquid or ice held aloft by the air
    float rain = 0.0F;  // falling out of it
    Vec3 wind{0.0F};    // m/s, in the planet's frame
};

// The air over a planet, and the ground it lies on.
struct Field {
    Grid grid;
    std::vector<Air> air;
    std::vector<double> ground; // per column: metres from the centre, the sea's level over sea
    std::vector<std::uint8_t> over_sea; // per column: whether the sea lies under the air

    [[nodiscard]] Air& at(std::size_t column, int layer) { return air[grid.index(column, layer)]; }
    [[nodiscard]] const Air& at(std::size_t column, int layer) const {
        return air[grid.index(column, layer)];
    }
    // Whether the ground fills the cell: its middle is under it.
    [[nodiscard]] bool solid(std::size_t column, int layer) const {
        return grid.radius_of(layer) < ground[column];
    }
};

// How fast still air cools with height, K/m: a share of the dry adiabat g/cₚ, as water vapour
// condensing in rising air slows it.
[[nodiscard]] double lapse_rate(const globe::Planet& planet);

// Mass of its vapour per kilogram of dry air at saturation, at a temperature and pressure.
[[nodiscard]] double saturation_ratio(globe::Volatile volatiles, double temperature,
                                      double pressure);

// Pressure `height` metres over the reference level, in air that cools with height at the lapse
// rate from `surface_temperature` there.
[[nodiscard]] double pressure_at(const globe::Planet& planet, double surface_temperature,
                                 double height);

// Still air, cooling with height from the climate's temperature at each latitude, holding a share
// of the vapour it could: more over the sea than the land. The weather is what it settles into.
[[nodiscard]] Field start_field(const globe::Generator& generator, int level, int layers);

} // namespace hog::weather
