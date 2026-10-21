#include <hog/waves/tide.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>

namespace hog::waves {

namespace {

constexpr double two_pi = 2.0 * std::numbers::pi;

// How far through a day of `day` ticks it is at longitude 0, kept exact however long since the
// epoch.
double through(GameTime time, std::int64_t day) noexcept {
    std::int64_t into = time.time_since_epoch().count() % day;
    if (into < 0) {
        into += day;
    }
    return static_cast<double>(into) / static_cast<double>(day);
}

} // namespace

double tide_at(DVec3 up, GameTime time, GameDuration day) noexcept {
    if (day.count() <= 0) {
        return 0.0;
    }
    const DVec3 unit = glm::normalize(up);
    const double longitude = std::atan2(unit.y, unit.x);
    // Where each stands overhead: the sun as sim::Calendar has it, over the far side of the planet
    // at midnight on longitude 0; and the moon, starting with it at the epoch.
    constexpr auto parts = 10'000;
    const auto moon_day = (day.count() * std::llround(lunar_day * parts)) / std::int64_t{parts};
    const double sun = std::numbers::pi - (two_pi * through(time, day.count()));
    const double moon = std::numbers::pi - (two_pi * through(time, moon_day));
    // The tide's highest where it's drawn up, under each and on the far side, and falls away to
    // none at the poles, smoothly round them; and the shape of the coasts makes it higher in some
    // seas than others, and later.
    const double drawn = 1.0 - (unit.z * unit.z);
    const double shaped = 1.0 + (0.5 * std::sin((3.0 * unit.x) + (2.0 * unit.z)));
    const double later = 1.5 * std::sin((2.0 * unit.y) - unit.z);
    return drawn * shaped *
           ((lunar_tide * std::cos((2.0 * (longitude - moon)) - later)) +
            (solar_tide * std::cos((2.0 * (longitude - sun)) - later)));
}

} // namespace hog::waves
