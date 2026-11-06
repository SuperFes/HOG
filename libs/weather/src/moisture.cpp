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
// Cloud water a kilogram of air holds before drops begin to grow, and how fast they do (Kessler).
constexpr double cloud_limit = 1e-3;
// Where ice grows at the drops' cost, cloud rains out at a fraction of that limit: from freezing
// down to this temperature it eases to `ice_limit` of it (Bergeron).
constexpr double freezing_point = 273.15;
constexpr double all_ice = 250.0;
constexpr double ice_limit = 0.25;
constexpr double conversion = 1e-3; // per second
constexpr double collection = 2.2;  // drops sweep up cloud as they fall
// How fast drops of a given water content fall, m/s: about 5 for light rain, 9 for heavy.
constexpr double fall_speed = 9.0;
constexpr double fall_reference = 2e-3;
// Seconds a drop takes to evaporate in air that holds none of the vapour it could.
constexpr double evaporation_time = 600.0;
constexpr double gas_constant = 8.314'462;
constexpr double air_molar_mass = 0.028'96;

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
    const auto latent = globe::latent_heat(planet.volatiles);
    if (!latent) {
        return;
    }
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
                            field.grid.depth_of(layer);
        const auto vapour = static_cast<double>(air.vapour);
        if (held > vapour) {
            const double gained = (held - vapour) * (1.0 - std::exp(-rate * seconds));
            air.vapour = static_cast<float>(vapour + gained);
            // The water took its latent heat from the surface it left.
            const double mass = air_density(static_cast<double>(air.temperature), pressure) *
                                field.grid.depth_of(layer);
            field.surface[column] -=
                static_cast<float>(gained * mass * *latent / surface_heat(field, column));
        }
    }
}

void exchange_heat(Field& field, const globe::Planet& planet, double seconds) {
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
        const double target = air_temperature(planet, surface, height);
        const double wind = std::max(least_wind, static_cast<double>(glm::length(air.wind)));
        const double rate = transfer * wind / field.grid.depth_of(layer);
        const double change =
            (target - static_cast<double>(air.temperature)) * (1.0 - std::exp(-rate * seconds));
        air.temperature = static_cast<float>(static_cast<double>(air.temperature) + change);
        // Joules per square metre the air took in, which the surface lost.
        const double pressure = pressure_at(planet, surface, height);
        const double mass = air_density(static_cast<double>(air.temperature), pressure) *
                            field.grid.depth_of(layer);
        field.surface[column] -=
            static_cast<float>(change * mass * air_heat_capacity() / surface_heat(field, column));
    }
}

void precipitate(Field& field, const globe::Planet& planet, double seconds) {
    const auto latent = globe::latent_heat(planet.volatiles);
    std::ranges::fill(field.precipitation, 0.0F);
    if (!latent) {
        return;
    }
    const double warming = *latent / air_heat_capacity();
    const auto layers = static_cast<std::size_t>(field.grid.layers);
    for (std::size_t column = 0; column < field.grid.columns(); ++column) {
        const auto surface = static_cast<double>(field.surface[column]);
        // Bottom up, so rain falls one cell at most in a step.
        for (int layer = 0; layer < field.grid.layers; ++layer) {
            const std::size_t cell = (column * layers) + static_cast<std::size_t>(layer);
            if (field.blocked[cell] != 0) {
                continue;
            }
            Air& air = field.air[cell];
            const double height = field.grid.radius_of(layer) - planet.radius;
            const double pressure = pressure_at(planet, surface, height);
            const auto temperature = static_cast<double>(air.temperature);
            auto cloud = static_cast<double>(air.cloud);
            auto rain = static_cast<double>(air.rain);

            const double iced = std::clamp((freezing_point - temperature) / (freezing_point - all_ice), 0.0, 1.0);
            const double limit = cloud_limit * (1.0 - ((1.0 - ice_limit) * iced));
            const double grown = std::max(0.0, cloud - limit) * conversion * seconds;
            const double swept = collection * cloud * std::pow(rain, 0.875) * seconds;
            const double made = std::min(cloud, grown + swept);
            cloud -= made;
            rain += made;

            const double held = saturation_ratio(planet.volatiles, temperature, pressure);
            const auto vapour = static_cast<double>(air.vapour);
            if (held > vapour && rain > 0.0) {
                const double lost =
                    std::min({rain, held - vapour,
                              rain * ((held - vapour) / held) * seconds / evaporation_time});
                rain -= lost;
                air.vapour = static_cast<float>(vapour + lost);
                air.temperature = static_cast<float>(temperature - (warming * lost));
            }

            const double density = pressure / (gas_constant / air_molar_mass * temperature);
            const double speed = fall_speed * std::pow(rain / fall_reference, 0.136);
            const double share = std::min(1.0, speed * seconds / field.grid.depth_of(layer));
            const double falling = rain * share;
            rain -= falling;
            air.cloud = static_cast<float>(cloud);
            air.rain = static_cast<float>(rain);
            if (layer == 0 || field.blocked[cell - 1] != 0) {
                // Metres of water, then mm over the hour.
                field.precipitation[column] += static_cast<float>(
                    falling * density * field.grid.depth_of(layer) / seconds * 3'600.0);
            } else {
                // What it carries to the cell below is the same mass of water in that cell's air.
                Air& below = field.air[cell - 1];
                const double under =
                    pressure_at(planet, surface, height - field.grid.between(layer - 1));
                const double thick = under / (gas_constant / air_molar_mass *
                                              static_cast<double>(below.temperature));
                // Cells further from the centre are bigger, so the same water is a thicker share
                // of the smaller cell under it.
                const double from = field.grid.radius_of(layer);
                const double to = field.grid.radius_of(layer - 1);
                below.rain += static_cast<float>(falling * density * field.grid.depth_of(layer) *
                                                 from * from /
                                                 (thick * field.grid.depth_of(layer - 1) * to * to));
            }
        }
    }
}

} // namespace hog::weather
