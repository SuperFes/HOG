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
    std::vector<std::uint8_t> blocked;  // per cell: whether the ground fills it
    std::vector<float> surface; // per column: its temperature reduced to the reference level

    [[nodiscard]] Air& at(std::size_t column, int layer) { return air[grid.index(column, layer)]; }
    [[nodiscard]] const Air& at(std::size_t column, int layer) const {
        return air[grid.index(column, layer)];
    }
    // Whether the ground fills the cell: its middle is under it.
    [[nodiscard]] bool solid(std::size_t column, int layer) const {
        return blocked[grid.index(column, layer)] != 0;
    }
};

// How fast a parcel of air cools as it rises, K/m: g/cₚ. Air that cools slower than this is stable.
[[nodiscard]] double dry_lapse_rate(const globe::Planet& planet);

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

// Metres over which still air at a temperature thins by e: RT/g.
[[nodiscard]] double scale_height(const globe::Planet& planet, double temperature);

// Air at a height over the reference level, over a surface at `surface` kelvin there: cooling at
// the lapse rate.
[[nodiscard]] double air_temperature(const globe::Planet& planet, double surface, double height);

// Sets a column's air still, cooling with height from its surface's temperature, with the share
// of the vapour it could hold that its ground leaves it.
void set_air(Field& field, const globe::Planet& planet, std::size_t column);

// Still air over a surface at the climate's temperature, holding a share of the vapour it could:
// more over the sea than the land. The weather is what it settles into.
[[nodiscard]] Field start_field(const globe::Generator& generator, int level, int layers);

// Heats and cools each column's surface over `seconds` with the sun towards the unit vector `sun`:
// it absorbs the sunlight that reaches it and radiates as a black body, holding heat by what it
// is, the sea deep and the land shallow. The sunlight is what keeps the planet's mean climate in
// balance. Heat that air and sea carry between latitudes is stood in for by a pull towards the
// climate, until winds carry it.
void warm_surface(Field& field, const globe::Planet& planet, DVec3 sun, double seconds);

} // namespace hog::weather
