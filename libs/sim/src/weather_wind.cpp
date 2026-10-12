#include "weather_stages.hpp"

#include <cmath>
#include <numbers>

namespace hog::sim::detail {

namespace {

constexpr double sea_level_pressure = 1'013.0; // hPa
constexpr double air_density = 1.2;            // kg/m³
constexpr double gas_constant = 287.0;         // dry air, J/(kg·K)

DVec2 capped(DVec2 wind, double limit) {
    const double speed = glm::length(wind);
    return speed > limit ? wind * (limit / speed) : wind;
}

} // namespace

void pressure(const StepContext& step, WeatherFields& fields, std::uint32_t index) {
    const WindTuning& tuning = step.config->tuning.wind;
    const Ground& ground = step.ground[index];
    const double sin_latitude = step.grid->frame(index).up.z;
    const Targets target =
        targets(step.config->tuning.heat, ground, sin_latitude, step.sin_declination);
    const double warmth = (sea_level(static_cast<double>(fields.temperature[index]), ground) -
                           static_cast<double>(fields.upper_temperature[index])) -
                          (target.reference - target.upper);
    fields.pressure[index] = static_cast<float>(
        sea_level_pressure - (tuning.band_pressure * std::cos(6.0 * std::asin(sin_latitude))) -
        (tuning.thermal_pressure * warmth));
}

void winds(const StepContext& step, WeatherFields& fields, std::uint32_t index) {
    const WindTuning& tuning = step.config->tuning.wind;
    const auto& around = step.grid->neighbors(index);
    const auto& weights = step.grid->gradient_weights(index);
    DVec2 pressure_gradient{0.0};
    DVec2 upper_gradient{0.0};
    for (std::size_t k = 0; k < around.size(); ++k) {
        pressure_gradient +=
            weights[k] * static_cast<double>(fields.pressure[around[k]] - fields.pressure[index]);
        upper_gradient += weights[k] * static_cast<double>(fields.upper_temperature[around[k]] -
                                                           fields.upper_temperature[index]);
    }
    // Per metre, in Pa and °C.
    pressure_gradient *= 100.0 / earth_radius;
    upper_gradient /= earth_radius;

    const double rotation = 2.0 * std::numbers::pi / Seconds{step.config->day}.count();
    const double sin_latitude = step.grid->frame(index).up.z;
    const double coriolis = 2.0 * rotation * sin_latitude;
    const double friction = step.ground[index].ocean ? tuning.ocean_friction : tuning.land_friction;
    // Steady balance of the pressure force, the Coriolis force and friction.
    const DVec2 push = -pressure_gradient / air_density;
    const DVec2 turned{-push.y, push.x};
    const DVec2 wind = capped(((friction * push) - (coriolis * turned)) /
                                  ((friction * friction) + (coriolis * coriolis)),
                              tuning.max_wind);

    // The thermal wind: the jet blows with cold air on its left in the north, on its right in
    // the south.
    const double floor = 2.0 * rotation * std::sin(tuning.min_coriolis_latitude * degree);
    const double held = std::abs(coriolis) < floor ? std::copysign(floor, sin_latitude) : coriolis;
    const DVec2 shear =
        gas_constant * std::numbers::ln2 / held * DVec2{-upper_gradient.y, upper_gradient.x};
    fields.wind[index] = Vec2{wind};
    fields.jet[index] = Vec2{capped(wind + shear, tuning.max_jet)};
}

} // namespace hog::sim::detail
