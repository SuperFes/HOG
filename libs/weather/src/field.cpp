#include <hog/weather/field.hpp>

#include <glm/geometric.hpp>

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

constexpr double stefan_boltzmann = 5.670'374e-8; // W/m²/K⁴
// Heat a square metre of surface holds per kelvin: the sea's mixed layer, about 50 m of water, and
// the land's annual skin, about 2.5 m of soil.
constexpr double sea_heat = 50.0 * 4.1e6;
constexpr double land_heat = 2.5 * 2.0e6;
// W/m² of heat carried towards the climate per kelvin away from it, about what Earth's air and
// seas carry between latitudes.
constexpr double transport = 2.0;

double latitude_of(DVec3 up) {
    return std::asin(std::clamp(up.z, -1.0, 1.0));
}

} // namespace

double air_heat_capacity() {
    return heat_capacity;
}

double dry_lapse_rate(const globe::Planet& planet) {
    return planet.gravity / heat_capacity;
}

double lapse_rate(const globe::Planet& planet) {
    return moist_share * dry_lapse_rate(planet);
}

double saturation_ratio(globe::Volatile volatiles, double temperature, double pressure) {
    const auto held = globe::saturation_pressure(volatiles, temperature);
    const auto mass = globe::molar_mass(volatiles);
    if (!held || !mass || pressure <= *held) {
        return 0.0;
    }
    return *mass / air_molar_mass * *held / (pressure - *held);
}

double scale_height(const globe::Planet& planet, double temperature) {
    return gas_constant / air_molar_mass * temperature / planet.gravity;
}

double air_temperature(const globe::Planet& planet, double surface, double height) {
    return std::max(surface - (lapse_rate(planet) * height), 1.0);
}

double pressure_at(const globe::Planet& planet, double surface_temperature, double height) {
    const double lapse = lapse_rate(planet);
    const double temperature = std::max(surface_temperature - (lapse * height), 1.0);
    const double specific = gas_constant / air_molar_mass;
    return planet.pressure *
           std::pow(temperature / surface_temperature, planet.gravity / (lapse * specific));
}

void set_air(Field& field, const globe::Planet& planet, std::size_t column) {
    const Grid& grid = field.grid;
    const auto surface = static_cast<double>(field.surface[column]);
    const double humidity = field.over_sea[column] != 0 ? sea_humidity : land_humidity;
    for (int layer = 0; layer < grid.layers; ++layer) {
        const double height = grid.radius_of(layer) - planet.radius;
        const double temperature = air_temperature(planet, surface, height);
        const double pressure = pressure_at(planet, surface, height);
        Air& air = field.at(column, layer);
        air.temperature = static_cast<float>(temperature);
        air.vapour = static_cast<float>(humidity *
                                        saturation_ratio(planet.volatiles, temperature, pressure));
    }
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
    field.blocked.resize(grid.size());
    field.surface.resize(grid.columns());

    // About a cell across, for the ground to be smoothed to.
    const double width = std::numbers::pi * 0.5 * planet.radius / static_cast<double>(1 << level);
    for (std::size_t column = 0; column < grid.columns(); ++column) {
        const DVec3 up = globe::center(globe::cell_at_index(level, column));
        const double ground = generator.column(up, width).ground();
        const bool wet = sea && ground < *sea;
        field.ground[column] = wet ? *sea : ground;
        field.over_sea[column] = wet ? 1 : 0;
        for (int layer = 0; layer < grid.layers; ++layer) {
            field.blocked[grid.index(column, layer)] =
                grid.radius_of(layer) < field.ground[column] ? 1 : 0;
        }
        field.surface[column] = static_cast<float>(globe::temperature_at(planet, latitude_of(up)));
        set_air(field, planet, column);
    }
    return field;
}

void warm_surface(Field& field, const globe::Planet& planet, DVec3 sun, double seconds) {
    // Averaged over the sphere the sun lights a quarter of the area facing it, so what the
    // planet's mean temperature radiates is a quarter of what the sun brings.
    const double mean = globe::mean_temperature(planet);
    const double absorbed = 4.0 * stefan_boltzmann * mean * mean * mean * mean;
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        const DVec3 up = globe::center(globe::cell_at_index(field.grid.level, column));
        const double climate = globe::temperature_at(planet, latitude_of(up));
        const double heat = field.over_sea[column] != 0 ? sea_heat : land_heat;
        const double lit = absorbed * std::max(0.0, glm::dot(up, sun));
        auto temperature = static_cast<double>(field.surface[column]);
        // Steps short of the time the surface takes to answer, so the sum stays stable.
        const double step =
            0.25 * heat / (4.0 * stefan_boltzmann * temperature * temperature * temperature);
        double left = seconds;
        while (left > 0.0) {
            const double dt = std::min(left, step);
            const double net = lit - (stefan_boltzmann * std::pow(temperature, 4.0)) +
                               (transport * (climate - temperature));
            temperature += net * dt / heat;
            left -= dt;
        }
        field.surface[column] = static_cast<float>(temperature);
    }
}

} // namespace hog::weather
