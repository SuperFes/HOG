#include <hog/core/assert.hpp>
#include <hog/planet/ephemeris.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <utility>

namespace hog::planet {

namespace {

constexpr double two_pi = 2.0 * std::numbers::pi;
constexpr double degree = std::numbers::pi / 180.0;

// J2000.0, noon on 1 January 2000, taking Terrestrial Time as UTC, as sim::Calendar does.
constexpr GameTime j2000{std::chrono::seconds{946'728'000}};
// The Julian year, which orbital elements are counted in.
constexpr double year_seconds = 365.25 * 86'400.0;

[[nodiscard]] double eccentric_anomaly(double mean, double eccentricity) noexcept {
    // Newton's method from the mean anomaly stalls on very eccentric orbits; from π it doesn't.
    double anomaly = eccentricity < 0.8 ? mean : std::numbers::pi;
    for (int step = 0; step < 50; ++step) {
        const double change = (anomaly - (eccentricity * std::sin(anomaly)) - mean) /
                              (1.0 - (eccentricity * std::cos(anomaly)));
        anomaly -= change;
        if (std::abs(change) < 1e-13) {
            break;
        }
    }
    return anomaly;
}

[[nodiscard]] double seconds_since_j2000(GameTime time) noexcept {
    return std::chrono::duration<double>(time - j2000).count();
}

// Black-body radiance at `wavelength` metres, to a constant factor.
[[nodiscard]] double planck(double wavelength, double temperature) noexcept {
    constexpr double second_radiation = 1.438'776'877e-2; // hc/k, metre kelvins
    return 1.0 /
           (std::pow(wavelength, 5.0) * std::expm1(second_radiation / (wavelength * temperature)));
}

[[nodiscard]] DVec3 radiance(double temperature) noexcept {
    // Near where the eye's red, green and blue cones see best.
    return {planck(610e-9, temperature), planck(550e-9, temperature), planck(465e-9, temperature)};
}

} // namespace

DVec3 orbit_position(const Orbit& orbit, double periods) noexcept {
    if (orbit.distance <= 0.0) {
        return {};
    }
    const double e = orbit.eccentricity;
    const double mean =
        std::remainder(((orbit.longitude - orbit.periapsis) * degree) + (two_pi * periods), two_pi);
    const double anomaly = eccentric_anomaly(mean, e);
    const double along = orbit.distance * (std::cos(anomaly) - e);
    const double across = orbit.distance * std::sqrt(1.0 - (e * e)) * std::sin(anomaly);

    const double node = orbit.node * degree;
    const double argument = (orbit.periapsis - orbit.node) * degree;
    const double inclination = orbit.inclination * degree;
    const double x = (std::cos(argument) * along) - (std::sin(argument) * across);
    const double y = (std::sin(argument) * along) + (std::cos(argument) * across);
    const double raised = y * std::cos(inclination);
    return {(std::cos(node) * x) - (std::sin(node) * raised),
            (std::sin(node) * x) + (std::cos(node) * raised), y * std::sin(inclination)};
}

DVec3 star_colour(double temperature) noexcept {
    const DVec3 colour = radiance(temperature) / radiance(Star{}.temperature);
    return colour / glm::dot(colour, DVec3{0.2126, 0.7152, 0.0722});
}

Ephemeris::Ephemeris(System system, GameDuration day_length, double axial_tilt)
    : system_(std::move(system)), day_length_(day_length), tilt_(axial_tilt * degree) {
    HOG_ASSERT(day_length.count() > 0, "a day must have a length");
    HOG_ASSERT(axial_tilt >= 0.0 && axial_tilt <= 90.0, "axial tilt out of range");
}

DVec3 Ephemeris::planet_around_centre(GameTime time) const noexcept {
    return orbit_position(system_.orbit,
                          seconds_since_j2000(time) / year_seconds / system_.orbit.period);
}

DVec3 Ephemeris::turned(DVec3 ecliptic, GameTime time) const noexcept {
    const DVec3 equatorial{ecliptic.x,
                           (ecliptic.y * std::cos(tilt_)) - (ecliptic.z * std::sin(tilt_)),
                           (ecliptic.y * std::sin(tilt_)) + (ecliptic.z * std::cos(tilt_))};

    // The calendar's day is a mean solar one: midnight at longitude 0 faces away from where the
    // centre would stand on a circular orbit.
    GameDuration::rep into = time.time_since_epoch().count() % day_length_.count();
    if (into < 0) {
        into += day_length_.count();
    }
    const double time_of_day = static_cast<double>(into) / static_cast<double>(day_length_.count());
    const double years = seconds_since_j2000(time) / year_seconds;
    const double mean_centre = (system_.orbit.longitude * degree) + std::numbers::pi +
                               (two_pi * years / system_.orbit.period);
    const double turn = mean_centre - (std::numbers::pi - (two_pi * time_of_day));
    return {(equatorial.x * std::cos(turn)) + (equatorial.y * std::sin(turn)),
            (equatorial.y * std::cos(turn)) - (equatorial.x * std::sin(turn)), equatorial.z};
}

DVec3 Ephemeris::star(std::size_t index, GameTime time) const {
    HOG_ASSERT(index < system_.stars.size(), "no such star");
    const Orbit& orbit = system_.stars[index].orbit;
    const DVec3 around =
        orbit_position(orbit, seconds_since_j2000(time) / year_seconds / orbit.period);
    return turned(around - planet_around_centre(time), time);
}

DVec3 Ephemeris::moon(std::size_t index, GameTime time) const {
    HOG_ASSERT(index < system_.moons.size(), "no such moon");
    // Its period runs from new moon to new, so it gains on the centre as the centre seems to
    // move round the sky.
    Orbit orbit = system_.moons[index].orbit;
    const double years = seconds_since_j2000(time) / year_seconds;
    orbit.longitude += 360.0 * years / system_.orbit.period;
    const double days =
        std::chrono::duration<double>(time - j2000) / std::chrono::duration<double>(day_length_);
    return turned(orbit_position(orbit, days / orbit.period), time);
}

DVec3 Ephemeris::planet(std::size_t index, GameTime time) const {
    HOG_ASSERT(index < system_.planets.size(), "no such planet");
    const Orbit& orbit = system_.planets[index].orbit;
    const DVec3 around =
        orbit_position(orbit, seconds_since_j2000(time) / year_seconds / orbit.period);
    return turned(around - planet_around_centre(time), time);
}

double Ephemeris::star_tide(std::size_t index, GameTime time) const {
    // A star the planet orbits is usually as far as the planet's orbit; one farther out, as far
    // as its own.
    const Star& star = system_.stars[index];
    const double usual = std::max(star.orbit.distance, system_.orbit.distance);
    return star.tide * std::pow(usual / glm::length(this->star(index, time)), 3.0);
}

double Ephemeris::moon_tide(std::size_t index, GameTime time) const {
    const Moon& moon = system_.moons[index];
    return moon.tide * std::pow(moon.orbit.distance / glm::length(this->moon(index, time)), 3.0);
}

double Ephemeris::irradiance(std::size_t index, GameTime time) const {
    const double distance = glm::length(star(index, time)) / astronomical_unit;
    return solar_constant * system_.stars[index].luminosity / (distance * distance);
}

} // namespace hog::planet
