#include <hog/client/daylight.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace hog::client {

namespace {

constexpr DVec3 night_glow{0.0015, 0.0018, 0.0025};
// Noon's light on level ground is about 1, seen at this exposure.
constexpr double noon_exposure = 10.0;
// How far exposure follows the light, from 0 not at all to 1 wholly, and the least light it
// follows down to.
constexpr double adaptation = 0.75;
constexpr double darkest = 0.002;
constexpr double adapting_seconds = 0.5;

double luminance(DVec3 light) noexcept {
    return (0.2126 * light.r) + (0.7152 * light.g) + (0.0722 * light.b);
}

} // namespace

void PlanetClock::advance(double seconds) noexcept {
    carried_ += std::max(seconds, 0.0) * rate_ * 1e6;
    const double whole = std::floor(carried_);
    carried_ -= whole;
    time_ += GameDuration{static_cast<GameDuration::rep>(whole)};
}

void PlanetClock::faster() noexcept {
    rate_ = std::min(rate_ * 2.0, fastest_time);
}

void PlanetClock::slower() noexcept {
    rate_ = std::max(rate_ / 2.0, 1.0);
}

GameTime start_time(const sim::Calendar& calendar, GameTime now, DVec3 up,
                    std::optional<double> time_of_day) {
    if (!time_of_day) {
        return now;
    }
    const double ahead = *time_of_day - calendar.local_time_of_day(now, up);
    const double share = ahead - std::floor(ahead);
    const std::chrono::duration<double, GameDuration::period> wait{
        share * static_cast<double>(calendar.day_length().count())};
    return now + std::chrono::round<GameDuration>(wait);
}

flora::Daylight daylight_at(const sim::Calendar& calendar, GameTime time, DVec3 up) {
    return {.hours = static_cast<float>(calendar.daylight_hours(time, up)),
            .longest = static_cast<float>(calendar.longest_daylight_hours(up)),
            .lengthening = sim::Calendar::days_lengthening(time, up)};
}

EyeLight light_at_eye(const atmosphere::Model& model, double height, double sun_mu) {
    const double mu = std::clamp(sun_mu, -1.0, 1.0);
    const DVec3 sun{std::sqrt(1.0 - (mu * mu)), 0.0, mu};
    return {.sun = model.transmittance(height, mu) * std::max(mu, 0.0),
            .sky = model.sky_irradiance(height, sun) + night_glow};
}

EyeLight under_cloud(const EyeLight& light, render::CloudShade shade) noexcept {
    return {.sun = light.sun * shade.direct,
            .sky = (light.sky * (shade.direct + shade.diffuse)) + (light.sun * shade.diffuse)};
}

double exposure_for(const EyeLight& light) noexcept {
    const double seen = std::max(luminance(light.sun + light.sky), darkest);
    return noon_exposure * std::pow(1.0 / seen, adaptation);
}

double adapted(double from, double to, double seconds) noexcept {
    const double share = 1.0 - std::exp(-std::max(seconds, 0.0) / adapting_seconds);
    return from * std::pow(to / from, share);
}

} // namespace hog::client
