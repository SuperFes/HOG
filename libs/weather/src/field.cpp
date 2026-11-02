#include <hog/weather/field.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::weather {
namespace {

constexpr double gas_constant = 8.314'462;
// Dry air's mean molar mass, kg/mol; the planet doesn't say what its air is made of.
constexpr double air_molar_mass = 0.028'96;
// Diatomic gas: cₚ is 7/2 of its specific gas constant.
constexpr double heat_capacity = 3.5 * gas_constant / air_molar_mass;
// Still air in the troposphere cools at about this share of the dry rate.
constexpr double moist_share = 0.65;
// Relative humidity of the air a column starts with: over the sea it's fed, over land it isn't.
constexpr double sea_humidity = 0.8;
constexpr double land_humidity = 0.5;

} // namespace

double lapse_rate(const globe::Planet& planet) {
    return moist_share * planet.gravity / heat_capacity;
}

double saturation_ratio(globe::Volatile volatiles, double temperature, double pressure) {
    const auto held = globe::saturation_pressure(volatiles, temperature);
    const auto mass = globe::molar_mass(volatiles);
    if (!held || !mass || pressure <= *held) {
        return 0.0;
    }
    return *mass / air_molar_mass * *held / (pressure - *held);
}

double pressure_at(const globe::Planet& planet, double surface_temperature, double height) {
    const double lapse = lapse_rate(planet);
    const double temperature = std::max(surface_temperature - (lapse * height), 1.0);
    const double specific = gas_constant / air_molar_mass;
    return planet.pressure *
           std::pow(temperature / surface_temperature, planet.gravity / (lapse * specific));
}

Field start_field(const globe::Generator& generator, int level, int layers) {
    const globe::Planet& planet = generator.planet();
    const auto sea = generator.sea_level();
    const double bottom = sea.value_or(planet.radius + generator.shape().lowest);

    Field field;
    field.grid = {.level = level,
                  .layers = layers,
                  .bottom = bottom,
                  .thickness = (planet.radius + planet.sky_height - bottom) / layers};
    const Grid& grid = field.grid;
    field.air.resize(grid.size());
    field.ground.resize(grid.columns());
    field.over_sea.resize(grid.columns());

    // About a cell across, for the ground to be smoothed to.
    const double width = std::numbers::pi * 0.5 * planet.radius / static_cast<double>(1 << level);
    for (std::size_t column = 0; column < grid.columns(); ++column) {
        const DVec3 up = globe::center(globe::cell_at_index(level, column));
        const double ground = generator.column(up, width).ground();
        const bool wet = sea && ground < *sea;
        field.ground[column] = wet ? *sea : ground;
        field.over_sea[column] = wet ? 1 : 0;

        const double latitude = std::asin(std::clamp(up.z, -1.0, 1.0));
        const double surface = globe::temperature_at(planet, latitude);
        const double humidity = wet ? sea_humidity : land_humidity;
        for (int layer = 0; layer < grid.layers; ++layer) {
            const double height = grid.radius_of(layer) - planet.radius;
            const double temperature = std::max(surface - (lapse_rate(planet) * height), 1.0);
            const double pressure = pressure_at(planet, surface, height);
            Air& air = field.at(column, layer);
            air.temperature = static_cast<float>(temperature);
            air.vapour = static_cast<float>(
                humidity * saturation_ratio(planet.volatiles, temperature, pressure));
        }
    }
    return field;
}

} // namespace hog::weather
