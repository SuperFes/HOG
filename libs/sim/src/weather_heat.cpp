#include "weather_stages.hpp"

#include <algorithm>
#include <cmath>

namespace hog::sim::detail {

Seasons seasons(const WeatherConfig& config, GameTime time) {
    const double tilt = std::sin(config.axial_tilt * degree);
    const auto lag = std::chrono::round<GameDuration>(config.tuning.heat.ocean_lag);
    return Seasons{
        .now = tilt * std::sin(Calendar::solar_longitude(time)),
        .sea = tilt * std::sin(Calendar::solar_longitude(time - lag)),
    };
}

Targets targets(const WeatherTuning& tuning, const Ground& ground, double sin_latitude,
                Seasons seasons, double sea_temperature) {
    const HeatTuning& heat = tuning.heat;
    const double sin_declination = seasons.now;
    const double land = sin_declination * sin_latitude;
    const double sea = seasons.sea * sin_latitude;
    const double latitude = std::abs(std::asin(sin_latitude));
    const double summer = sin_latitude < 0.0 ? -sin_declination : sin_declination;
    const double middle = (heat.band_latitude + (heat.band_seasons * summer)) * degree;
    const double drop = heat.upper_drop - (heat.upper_drop_seasons * summer);
    const double cooling =
        0.5 + (0.5 * std::tanh((latitude - middle) / (heat.band_width * degree)));
    const auto mean = static_cast<double>(ground.mean_temperature);
    const double reference = mean + (heat.ocean_seasons * sea);
    // Ice keeps the air off the water under it, so the air over it follows the seasons.
    const double over_sea = sea_temperature > tuning.ocean.freezing
                                ? sea_temperature
                                : std::min(sea_temperature, reference);
    return Targets{
        .surface = ground.ocean ? over_sea : mean + (heat.land_seasons * land),
        .upper = heat.upper_equator - (drop * cooling),
        .reference = reference,
    };
}

void heat(const StepContext& step, WeatherFields& fields, std::uint32_t index) {
    const HeatTuning& tuning = step.config->tuning.heat;
    const Ground& ground = step.ground[index];
    const DVec3 up = step.grid->frame(index).up;
    const Targets target = targets(step.config->tuning, ground, up.z, step.seasons,
                                   static_cast<double>(step.sea[index]));
    const auto cover = static_cast<double>(fields.cloud_cover(index));
    const double sun = std::sin(step.calendar->sun_elevation(step.middle, up));
    const double heating = (ground.ocean ? tuning.ocean_days : tuning.land_days) * sun *
                           (1.0 - (tuning.cloud_shade * cover));
    const double surface_move =
        approach(step.seconds, ground.ocean ? tuning.ocean_response : tuning.land_response);

    auto surface = static_cast<double>(fields.temperature[index]);
    auto upper = static_cast<double>(fields.upper_temperature[index]);
    surface += (target.surface + heating - surface) * surface_move;
    upper += (target.upper - upper) * approach(step.seconds, tuning.upper_response);
    fields.temperature[index] = static_cast<float>(surface);
    fields.upper_temperature[index] = static_cast<float>(upper);
}

} // namespace hog::sim::detail
