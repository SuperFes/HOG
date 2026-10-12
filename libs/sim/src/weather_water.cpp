#include "weather_stages.hpp"

#include <algorithm>
#include <cmath>

namespace hog::sim::detail {

void water(const StepContext& step, WeatherFields& fields, std::uint32_t index) {
    const WaterTuning& tuning = step.config->tuning.water;
    const StormTuning& storms = step.config->tuning.storms;
    const Ground& ground = step.ground[index];
    const double storm_move = approach(step.seconds, storms.response);
    const LiftTuning& lifting = step.config->tuning.lift;
    // From -1 for strongly sinking air to 1 for strongly rising.
    const double rising = std::tanh(static_cast<double>(fields.lift[index]) / lifting.reference);

    auto surface = static_cast<double>(fields.temperature[index]);
    auto upper = static_cast<double>(fields.upper_temperature[index]);
    auto vapour = static_cast<double>(fields.vapour[index]);
    auto cloud = static_cast<double>(fields.cloud[index]);
    const double holds = saturation(surface);
    const double evaporating = std::min(tuning.evaporation_per_day * step.seconds / 86'400.0, 1.0);
    const double evaporation =
        std::max(holds - vapour, 0.0) * static_cast<double>(ground.wetness) * evaporating;
    vapour += evaporation;

    // Thunderstorms rain straight out of the air and stir the column towards neutral.
    const double cold_front = std::max(-static_cast<double>(fields.front[index]), 0.0);
    const double contrast = sea_level(surface, ground) + vapour - upper - storms.neutral_contrast +
                            (lifting.storm * std::max(rising, 0.0)) +
                            (step.config->tuning.fronts.cold_storm * cold_front);
    const double storm = std::clamp(contrast / storms.full_contrast, 0.0, 1.0);
    const double convected = storm * vapour * storms.rain_out * storm_move;
    vapour -= convected;
    cloud += convected * storms.anvil;
    const double mixed = storm * std::max(contrast, 0.0) * storms.mixing * storm_move;
    surface -= ground.ocean ? 0.0 : mixed * storms.land_cooling;
    upper += mixed;

    const double sinking = std::max(-rising, 0.0);
    const double limit = std::min(tuning.condensing_humidity - (lifting.humidity * rising), 1.0) *
                         saturation(surface + (lifting.foehn * sinking));
    if (vapour > limit) {
        cloud += vapour - limit;
        vapour = limit;
    } else {
        const double cleared = std::min(cloud, limit - vapour) *
                               approach(step.seconds * (1.0 + sinking), tuning.cloud_clearing);
        cloud -= cleared;
        vapour += cleared;
    }
    // Rising air rains its cloud out sooner.
    const double stratiform =
        std::max(cloud - tuning.raining_cloud, 0.0) *
        approach(step.seconds * (1.0 + std::max(rising, 0.0)), tuning.rain_out);
    cloud -= stratiform;

    const double storm_fall = convected * (1.0 - storms.anvil);
    const double hail =
        upper < storms.hail_below
            ? storm_fall * std::clamp((storm - storms.hail_storm) * 1.2, 0.0, storms.max_hail)
            : 0.0;
    const double falling = stratiform + storm_fall - hail;
    const double snowy = std::clamp((tuning.snow_below - surface) / 3.0, 0.0, 1.0);

    fields.temperature[index] = static_cast<float>(surface);
    fields.upper_temperature[index] = static_cast<float>(upper);
    fields.vapour[index] = static_cast<float>(vapour);
    fields.cloud[index] = static_cast<float>(cloud);
    fields.evaporation[index] = static_cast<float>(evaporation);
    fields.rain[index] = static_cast<float>(falling * (1.0 - snowy));
    fields.snow[index] = static_cast<float>(falling * snowy);
    fields.hail[index] = static_cast<float>(hail);
    fields.storm[index] = static_cast<float>(storm);
}

} // namespace hog::sim::detail
