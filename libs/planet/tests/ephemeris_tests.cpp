#include <hog/planet/ephemeris.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

using namespace std::chrono;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::GameTime;
using hog::planet::astronomical_unit;
using hog::planet::Ephemeris;
using hog::planet::Orbit;
using hog::planet::orbit_position;
using hog::planet::System;

namespace {

constexpr double degree = std::numbers::pi / 180.0;
constexpr hours four_hours{4};

GameTime at(sys_days day, minutes time = {}) {
    return hog::to_game_time(hog::WallClock::time_point{day + time});
}

double angle_between(DVec3 a, DVec3 b) {
    return std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0)) /
           degree;
}

Ephemeris earths() {
    return Ephemeris{System{}, four_hours, 23.44};
}

} // namespace

TEST_CASE("an orbit comes nearest at its periapsis and round again in its period",
          "[planet][ephemeris]") {
    const Orbit orbit{.distance = 1.0, .eccentricity = 0.5, .periapsis = 30.0, .longitude = 30.0};

    const DVec3 nearest = orbit_position(orbit, 0.0);
    CHECK_THAT(glm::length(nearest), WithinRel(0.5, 1e-12));
    CHECK_THAT(std::atan2(nearest.y, nearest.x) / degree, WithinAbs(30.0, 1e-9));

    const DVec3 farthest = orbit_position(orbit, 0.5);
    CHECK_THAT(glm::length(farthest), WithinRel(1.5, 1e-12));
    CHECK_THAT(angle_between(nearest, farthest), WithinAbs(180.0, 1e-6));

    CHECK_THAT(glm::length(orbit_position(orbit, 3.0) - nearest), WithinAbs(0.0, 1e-9));
    // Kepler's second law: quick near the star, slow far out.
    CHECK(angle_between(nearest, orbit_position(orbit, 0.1)) >
          angle_between(farthest, orbit_position(orbit, 0.6)));
}

TEST_CASE("an inclined orbit crosses the plane at its node and rises by its inclination",
          "[planet][ephemeris]") {
    const Orbit orbit{
        .distance = 2.0, .inclination = 30.0, .node = 40.0, .periapsis = 40.0, .longitude = 40.0};

    const DVec3 crossing = orbit_position(orbit, 0.0);
    CHECK_THAT(crossing.z, WithinAbs(0.0, 1e-12));
    CHECK_THAT(std::atan2(crossing.y, crossing.x) / degree, WithinAbs(40.0, 1e-9));
    CHECK_THAT(orbit_position(orbit, 0.25).z, WithinAbs(2.0 * std::sin(30.0 * degree), 1e-12));
    CHECK_THAT(orbit_position(orbit, 0.75).z, WithinAbs(-2.0 * std::sin(30.0 * degree), 1e-12));
}

TEST_CASE("a body at the centre stays there", "[planet][ephemeris]") {
    CHECK(orbit_position(Orbit{}, 0.3) == DVec3{});
}

TEST_CASE("the sun stands over the tropics at the solstices and the equator at the equinoxes",
          "[planet][ephemeris]") {
    const Ephemeris ephemeris = earths();
    const auto declination = [&](GameTime time) {
        return std::asin(glm::normalize(ephemeris.star(0, time)).z) / degree;
    };
    // 2026's, to the minute.
    CHECK_THAT(declination(at(2026y / March / 20, 14h + 46min)), WithinAbs(0.0, 0.02));
    CHECK_THAT(declination(at(2026y / June / 21, 8h + 24min)), WithinAbs(23.44, 0.02));
    CHECK_THAT(declination(at(2026y / September / 23, 0h + 5min)), WithinAbs(0.0, 0.02));
    CHECK_THAT(declination(at(2026y / December / 21, 20h + 50min)), WithinAbs(-23.44, 0.02));
}

TEST_CASE("the sun crosses longitude 0 at noon, within the slip of the seasons",
          "[planet][ephemeris]") {
    const Ephemeris ephemeris = earths();
    // The slip is the equation of time: up to a quarter hour of Earth's day, near 4 degrees.
    for (int day = 0; day < 365; day += 7) {
        const GameTime noon = at(2026y / January / 1) + (day * four_hours) + (four_hours / 2);
        const DVec3 sun = ephemeris.star(0, noon);
        CAPTURE(day);
        CHECK(std::abs(std::atan2(sun.y, sun.x) / degree) < 4.5);
    }
}

TEST_CASE("the moon is new again after a month of the planet's days", "[planet][ephemeris]") {
    const Ephemeris ephemeris = earths();
    const auto elongation = [&](GameTime time) {
        return angle_between(ephemeris.moon(0, time), ephemeris.star(0, time));
    };
    const GameTime start = at(2026y / March / 1);
    const auto synodic = duration_cast<hog::GameDuration>(29.53 * four_hours);
    CHECK_THAT(elongation(start + synodic), WithinAbs(elongation(start), 8.0));
    // Half a month on, it stands as far from full as it stood from new.
    CHECK_THAT(elongation(start + (synodic / 2)) + elongation(start), WithinAbs(180.0, 12.0));

    for (int day = 0; day < 30; ++day) {
        const double distance = glm::length(ephemeris.moon(0, start + (day * four_hours)));
        CHECK(distance > 3.56e8);
        CHECK(distance < 4.07e8);
    }
}

TEST_CASE("Venus keeps near the sun and Mars comes round opposite it", "[planet][ephemeris]") {
    const Ephemeris ephemeris = earths();
    double venus = 0.0;
    double mars = 0.0;
    for (int day = 0; day < 2 * 365; day += 2) {
        const GameTime time = at(2026y / January / 1) + days{day};
        const DVec3 sun = ephemeris.star(0, time);
        venus = std::max(venus, angle_between(ephemeris.planet(1, time), sun));
        mars = std::max(mars, angle_between(ephemeris.planet(2, time), sun));
    }
    CHECK(venus > 44.0);
    CHECK(venus < 48.0);
    CHECK(mars > 175.0);
}

TEST_CASE("a star's light falls off with the square of its distance", "[planet][ephemeris]") {
    const Ephemeris ephemeris = earths();
    const double january = ephemeris.irradiance(0, at(2026y / January / 3));
    const double july = ephemeris.irradiance(0, at(2026y / July / 6));
    CHECK_THAT(january, WithinRel(1'361.0 / std::pow(1.0 - 0.0167, 2.0), 1e-3));
    CHECK_THAT(july, WithinRel(1'361.0 / std::pow(1.0 + 0.0167, 2.0), 1e-3));

    System pair;
    pair.stars = {
        {.luminosity = 0.6, .orbit = {.distance = 0.05 * astronomical_unit, .period = 0.01}},
        {.luminosity = 0.4,
         .orbit = {.distance = 0.05 * astronomical_unit, .longitude = 180.0, .period = 0.01}}};
    pair.orbit.eccentricity = 0.0;
    const Ephemeris both{pair, four_hours, 23.44};
    const GameTime time = at(2026y / May / 1);
    CHECK_THAT(both.irradiance(0, time) + both.irradiance(1, time), WithinRel(1'361.0, 0.05));
    CHECK_THAT(glm::length(both.star(0, time) - both.star(1, time)),
               WithinRel(0.1 * astronomical_unit, 1e-9));
}

TEST_CASE("a star's colour follows how hot it is, the Sun's white", "[planet][ephemeris]") {
    const DVec3 sun = hog::planet::star_colour(5'772.0);
    CHECK_THAT(sun.r, WithinAbs(1.0, 1e-12));
    CHECK_THAT(sun.g, WithinAbs(1.0, 1e-12));
    CHECK_THAT(sun.b, WithinAbs(1.0, 1e-12));

    const DVec3 red_dwarf = hog::planet::star_colour(3'500.0);
    CHECK(red_dwarf.r > red_dwarf.g);
    CHECK(red_dwarf.g > red_dwarf.b);
    const DVec3 blue = hog::planet::star_colour(10'000.0);
    CHECK(blue.b > blue.g);
    CHECK(blue.g > blue.r);
}

TEST_CASE("a body raises the tide more the nearer it stands", "[planet][ephemeris]") {
    const Ephemeris ephemeris = earths();
    double weakest = 1.0;
    double strongest = 0.0;
    for (int day = 0; day < 60; ++day) {
        const double moon = ephemeris.moon_tide(0, at(2026y / March / 1) + (day * four_hours));
        weakest = std::min(weakest, moon);
        strongest = std::max(strongest, moon);
    }
    // The Moon comes 11 per cent nearer at perigee than at apogee, so its tide a third stronger.
    CHECK_THAT(weakest, WithinRel(0.6 / std::pow(1.0549, 3.0), 0.01));
    CHECK_THAT(strongest, WithinRel(0.6 / std::pow(0.9451, 3.0), 0.01));

    // The Sun is nearest in January.
    CHECK(ephemeris.star_tide(0, at(2026y / January / 3)) >
          ephemeris.star_tide(0, at(2026y / July / 6)));
    CHECK_THAT(ephemeris.star_tide(0, at(2026y / April / 4)), WithinRel(0.28, 0.01));
}
