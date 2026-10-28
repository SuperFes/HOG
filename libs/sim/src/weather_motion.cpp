#include "weather_stages.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace hog::sim::detail {

namespace {

double at(std::span<const float> field, std::uint32_t index) {
    return static_cast<double>(field[index]);
}

} // namespace

DVec3 departure(const planet::TangentFrame& frame, Vec2 velocity, double seconds, double radius) {
    const double speed = glm::length(DVec2{velocity});
    if (speed == 0.0) {
        return frame.up;
    }
    const double angle = speed * seconds / radius;
    const DVec3 heading = ((frame.east * static_cast<double>(velocity.x)) +
                           (frame.north * static_cast<double>(velocity.y))) /
                          speed;
    return (frame.up * std::cos(angle)) - (heading * std::sin(angle));
}

void move(const StepContext& step, const WeatherFields& before, WeatherFields& fields,
          std::uint32_t index) {
    const MotionTuning& tuning = step.config->tuning.motion;
    const CellGrid& grid = *step.grid;
    const planet::TangentFrame& frame = grid.frame(index);
    const Vec2 wind = before.wind[index];
    const Vec2 jet = before.jet[index];
    const Vec2 steering = wind + ((jet - wind) * static_cast<float>(tuning.jet_steering));

    const double radius = step.config->carry_radius;
    const DVec3 low = departure(frame, steering, step.seconds, radius);
    // Air keeps its warmth brought to sea level, so crossing mountains doesn't heat or chill it.
    const double surface = grid.interpolate(low, [&](std::uint32_t cell) {
        return sea_level(at(before.temperature, cell), step.ground[cell]);
    });
    fields.temperature[index] = static_cast<float>(surface - sea_level(0.0, step.ground[index]));
    fields.vapour[index] = static_cast<float>(
        grid.interpolate(low, [&](std::uint32_t cell) { return at(before.vapour, cell); }));
    fields.cloud[index] = static_cast<float>(
        grid.interpolate(low, [&](std::uint32_t cell) { return at(before.cloud, cell); }));

    const DVec3 high = departure(frame, jet, step.seconds, radius);
    fields.upper_temperature[index] = static_cast<float>(grid.interpolate(
        high, [&](std::uint32_t cell) { return at(before.upper_temperature, cell); }));
}

void conserve_water(const CellGrid& grid, const WeatherFields& before, WeatherFields& fields) {
    double had = 0.0;
    double has = 0.0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        had += grid.area(index) * (at(before.vapour, index) + at(before.cloud, index));
        has += grid.area(index) * (at(fields.vapour, index) + at(fields.cloud, index));
    }
    if (has <= 0.0) {
        return;
    }
    const auto scale = static_cast<float>(had / has);
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        fields.vapour[index] *= scale;
        fields.cloud[index] *= scale;
    }
}

} // namespace hog::sim::detail
