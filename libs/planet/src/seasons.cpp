#include <hog/planet/ephemeris.hpp>
#include <hog/planet/seasons.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace hog::planet {

namespace {

constexpr double degree = std::numbers::pi / 180.0;

// How far the cycles move what they swing over `years` from J2000.
[[nodiscard]] double drift(const std::vector<Cycle>& cycles, double years) noexcept {
    double moved = 0.0;
    for (const Cycle& cycle : cycles) {
        const double phase = cycle.phase * degree;
        moved += cycle.size * (std::cos((2.0 * std::numbers::pi * years / cycle.period) + phase) -
                               std::cos(phase));
    }
    return moved;
}

} // namespace

Seasons seasons_at(const System& system, double axial_tilt, double years) noexcept {
    const Cycles& cycles = system.cycles;
    const double turned = cycles.precession == 0.0 ? 0.0 : 360.0 * years / cycles.precession;
    const double periapsis = std::fmod(system.orbit.periapsis + turned, 360.0);
    return {.tilt = std::clamp(axial_tilt + drift(cycles.tilt, years), 0.0, 90.0),
            .eccentricity = std::clamp(
                system.orbit.eccentricity + drift(cycles.eccentricity, years), 0.0, 0.99),
            .periapsis = periapsis < 0.0 ? periapsis + 360.0 : periapsis};
}

double daily_sunlight(const System& system, const Seasons& seasons, double latitude,
                      double solar_longitude) noexcept {
    // Stars inside the planet's orbit light it as one; any farther out are only points.
    double luminosity = 0.0;
    for (const Star& star : system.stars) {
        if (star.orbit.distance < system.orbit.distance) {
            luminosity += star.luminosity;
        }
    }
    const double mean_distance = system.orbit.distance / astronomical_unit;
    const double flux = solar_constant * luminosity / (mean_distance * mean_distance);

    const double e = seasons.eccentricity;
    const double sun = solar_longitude * degree;
    // The planet stands at the periapsis when the sun, seen from it, stands opposite.
    const double anomaly = sun - ((seasons.periapsis + 180.0) * degree);
    const double nearness = (1.0 + (e * std::cos(anomaly))) / (1.0 - (e * e));

    const double sin_declination = std::sin(seasons.tilt * degree) * std::sin(sun);
    const double cos_declination = std::sqrt(1.0 - (sin_declination * sin_declination));
    const double sin_latitude = std::sin(latitude * degree);
    const double cos_latitude = std::cos(latitude * degree);
    // The sun's hour angle at setting, 0 through the polar night and π under the midnight sun.
    const double across = sin_latitude * sin_declination;
    const double scale = cos_latitude * cos_declination;
    double setting = across > 0.0 ? std::numbers::pi : 0.0;
    if (scale > 0.0) {
        setting = std::acos(std::clamp(-across / scale, -1.0, 1.0));
    }
    return flux * nearness * nearness / std::numbers::pi *
           ((setting * across) + (scale * std::sin(setting)));
}

} // namespace hog::planet
