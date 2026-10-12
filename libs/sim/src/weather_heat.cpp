#include "weather_stages.hpp"

#include <algorithm>
#include <cmath>

namespace hog::sim::detail {

double sin_declination(GameTime time, double axial_tilt) {
    return std::sin(axial_tilt * degree) * std::sin(Calendar::solar_longitude(time));
}

Targets targets(const HeatTuning& tuning, const Ground& ground, double sin_latitude,
                double sin_declination) {
    const double seasons = sin_declination * sin_latitude;
    const double latitude = std::abs(std::asin(sin_latitude));
    const double summer = sin_latitude < 0.0 ? -sin_declination : sin_declination;
    const double middle = (tuning.band_latitude + (tuning.band_seasons * summer)) * degree;
    const double drop = tuning.upper_drop - (tuning.upper_drop_seasons * summer);
    const double cooling =
        0.5 + (0.5 * std::tanh((latitude - middle) / (tuning.band_width * degree)));
    const auto mean = static_cast<double>(ground.mean_temperature);
    return Targets{
        .surface = mean + ((ground.ocean ? tuning.ocean_seasons : tuning.land_seasons) * seasons),
        .upper = tuning.upper_equator - (drop * cooling),
        .reference = mean + (tuning.ocean_seasons * seasons),
    };
}

void heat(const StepContext& step, WeatherFields& fields, std::uint32_t index) {
    const HeatTuning& tuning = step.config->tuning.heat;
    const Ground& ground = step.ground[index];
    const DVec3 up = step.grid->frame(index).up;
    const Targets target = targets(tuning, ground, up.z, step.sin_declination);
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
