#include <hog/weather/moisture.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace hog::weather {
namespace {

// Bulk transfer over open water: the share of a wind's speed the air picks up, and the lightest
// breeze there is, with eddies the grid can't see.
constexpr double transfer = 1.3e-3;
constexpr double least_wind = 3.0; // m/s
// Land is wet only where rain or water has been, which the weather doesn't yet know: a share of
// the sea's rate.
constexpr double land_wetness = 0.2;
constexpr double step_kelvin = 0.05;

double saturation(globe::Volatile volatiles, double temperature, double pressure) {
    return saturation_ratio(volatiles, temperature, pressure);
}

} // namespace

void saturate(Parcel& parcel, globe::Volatile volatiles, double latent_heat, double pressure) {
    const double warming = latent_heat / air_heat_capacity();
    // Newton's method on the water left as vapour, as condensing warms the air and so raises what
    // it can hold.
    for (int pass = 0; pass < 4; ++pass) {
        const double held = saturation(volatiles, parcel.temperature, pressure);
        const double slope =
            (saturation(volatiles, parcel.temperature + step_kelvin, pressure) - held) /
            step_kelvin;
        double change = (parcel.vapour - held) / (1.0 + (warming * slope));
        // Cloud to evaporate is limited to what there is.
        change = std::max(change, -parcel.cloud);
        if (std::abs(change) < 1e-12) {
            break;
        }
        parcel.vapour -= change;
        parcel.cloud += change;
        parcel.temperature += warming * change;
    }
}

void condense(Field& field, const globe::Planet& planet) {
    const auto latent = globe::latent_heat(planet.volatiles);
    if (!latent) {
        return;
    }
    const auto layers = static_cast<std::size_t>(field.grid.layers);
    for (std::size_t cell = 0; cell < field.air.size(); ++cell) {
        if (field.blocked[cell] != 0) {
            continue;
        }
        const std::size_t column = cell / layers;
        const int layer = static_cast<int>(cell % layers);
        Air& air = field.air[cell];
        const double pressure = pressure_at(planet, static_cast<double>(field.surface[column]),
                                            field.grid.radius_of(layer) - planet.radius);
        Parcel parcel{.temperature = static_cast<double>(air.temperature),
                      .vapour = static_cast<double>(air.vapour),
                      .cloud = static_cast<double>(air.cloud)};
        saturate(parcel, planet.volatiles, *latent, pressure);
        air.temperature = static_cast<float>(parcel.temperature);
        air.vapour = static_cast<float>(parcel.vapour);
        air.cloud = static_cast<float>(parcel.cloud);
    }
}

void evaporate(Field& field, const globe::Planet& planet, double seconds) {
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        int layer = 0;
        while (layer < field.grid.layers && field.solid(column, layer)) {
            ++layer;
        }
        if (layer == field.grid.layers) {
            continue;
        }
        Air& air = field.at(column, layer);
        const double height = field.grid.radius_of(layer) - planet.radius;
        const auto surface = static_cast<double>(field.surface[column]);
        const double pressure = pressure_at(planet, surface, height);
        // The air at the ground holds what the surface could, at the surface's temperature there.
        const double held =
            saturation(planet.volatiles, air_temperature(planet, surface, height), pressure);
        const double wind = std::max(least_wind, static_cast<double>(glm::length(air.wind)));
        const double rate = transfer * wind * (field.over_sea[column] != 0 ? 1.0 : land_wetness) /
                            field.grid.thickness;
        const auto vapour = static_cast<double>(air.vapour);
        if (held > vapour) {
            air.vapour =
                static_cast<float>(vapour + ((held - vapour) * (1.0 - std::exp(-rate * seconds))));
        }
    }
}

} // namespace hog::weather
