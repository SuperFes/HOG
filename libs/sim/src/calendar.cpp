#include <hog/core/assert.hpp>
#include <hog/sim/calendar.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace hog::sim {

namespace {

constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr double degree = std::numbers::pi / 180.0;

// J2000.0, noon on 1 January 2000, taking Terrestrial Time as UTC: a minute out, which moves an
// equinox by seconds.
constexpr GameTime j2000{std::chrono::seconds{946'728'000}};

[[nodiscard]] double wrap(double angle) {
    const double wrapped = std::fmod(angle, two_pi);
    return wrapped < 0.0 ? wrapped + two_pi : wrapped;
}

// Hours of 24 the sun is up at a latitude when it stands over another.
double hours_lit(double sin_latitude, double sin_declination) noexcept {
    // The sun's hour angle at setting has cos = -tan(latitude) tan(declination).
    const double across = -sin_latitude * sin_declination;
    const double scale = std::sqrt((1.0 - (sin_latitude * sin_latitude)) *
                                   (1.0 - (sin_declination * sin_declination)));
    if (across >= scale) {
        return 0.0;
    }
    if (across <= -scale) {
        return 24.0;
    }
    return 24.0 * std::acos(across / scale) / std::numbers::pi;
}

} // namespace

Calendar::Calendar(GameDuration day_length, double axial_tilt)
    : day_length_(day_length), sin_tilt_(std::sin(axial_tilt * degree)) {
    HOG_ASSERT(day_length.count() > 0, "a day must have a length");
    HOG_ASSERT(axial_tilt >= 0.0 && axial_tilt <= 90.0, "axial tilt out of range");
}

std::int64_t Calendar::day(GameTime time) const noexcept {
    const GameDuration::rep since = time.time_since_epoch().count();
    return (since / day_length_.count()) - (since % day_length_.count() < 0 ? 1 : 0);
}

double Calendar::time_of_day(GameTime time) const noexcept {
    GameDuration::rep into = time.time_since_epoch().count() % day_length_.count();
    if (into < 0) {
        into += day_length_.count();
    }
    return static_cast<double>(into) / static_cast<double>(day_length_.count());
}

double Calendar::local_time_of_day(GameTime time, DVec3 up) const noexcept {
    const double local = time_of_day(time) + (std::atan2(up.y, up.x) / two_pi);
    return local - std::floor(local);
}

// The Astronomical Almanac's low-precision formula, good to about 0.01 degrees, which places an
// equinox within a quarter of an hour.
double Calendar::solar_longitude(GameTime time) noexcept {
    const double days =
        std::chrono::duration<double, std::chrono::days::period>(time - j2000).count();
    const double mean_longitude = (280.460 + (0.9856474 * days)) * degree;
    const double mean_anomaly = (357.528 + (0.9856003 * days)) * degree;
    return wrap(mean_longitude + (1.915 * degree * std::sin(mean_anomaly)) +
                (0.020 * degree * std::sin(2.0 * mean_anomaly)));
}

Season Calendar::season(GameTime time, DVec3 up) noexcept {
    auto quarter = static_cast<int>(solar_longitude(time) / (std::numbers::pi / 2.0));
    quarter = std::min(quarter, 3);
    if (up.z < 0.0) {
        quarter = (quarter + 2) % 4;
    }
    return static_cast<Season>(quarter);
}

DVec3 Calendar::sun_direction(GameTime time) const noexcept {
    const double sin_declination = sin_tilt_ * std::sin(solar_longitude(time));
    const double cos_declination = std::sqrt(1.0 - (sin_declination * sin_declination));
    // At midnight on longitude 0 the sun is over the far side of the planet.
    const double longitude = std::numbers::pi - (two_pi * time_of_day(time));
    return {cos_declination * std::cos(longitude), cos_declination * std::sin(longitude),
            sin_declination};
}

double Calendar::sun_elevation(GameTime time, DVec3 up) const noexcept {
    return std::asin(std::clamp(glm::dot(sun_direction(time), glm::normalize(up)), -1.0, 1.0));
}

double Calendar::daylight_hours(GameTime time, DVec3 up) const noexcept {
    return hours_lit(glm::normalize(up).z, sin_tilt_ * std::sin(solar_longitude(time)));
}

double Calendar::longest_daylight_hours(DVec3 up) const noexcept {
    return hours_lit(std::abs(glm::normalize(up).z), sin_tilt_);
}

bool Calendar::days_lengthening(GameTime time, DVec3 up) noexcept {
    const bool rising = std::cos(solar_longitude(time)) > 0.0;
    return rising == (up.z >= 0.0);
}

double sunlight(double elevation) noexcept {
    const double share = std::clamp((elevation + twilight) / twilight, 0.0, 1.0);
    return share * share * (3.0 - (2.0 * share));
}

} // namespace hog::sim
