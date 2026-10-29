#include <hog/planet/seasons.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::planet::daily_sunlight;
using hog::planet::Seasons;
using hog::planet::seasons_at;
using hog::planet::System;

namespace {

constexpr double degree = std::numbers::pi / 180.0;
constexpr double tilt = 23.44;

} // namespace

TEST_CASE("the seasons today are the planet's own", "[planet][seasons]") {
    const System system;
    const Seasons today = seasons_at(system, tilt, 0.0);
    CHECK_THAT(today.tilt, WithinAbs(tilt, 1e-12));
    CHECK_THAT(today.eccentricity, WithinAbs(system.orbit.eccentricity, 1e-12));
    CHECK_THAT(today.periapsis, WithinAbs(system.orbit.periapsis, 1e-12));
}

TEST_CASE("the tilt and the orbit's stretch swing within Earth's bounds", "[planet][seasons]") {
    const System system;
    double lowest_tilt = 90.0;
    double highest_tilt = 0.0;
    double roundest = 1.0;
    double longest = 0.0;
    int crossings = 0;
    bool above = true;
    for (int century = 0; century < 10'000; ++century) {
        const Seasons then = seasons_at(system, tilt, -100.0 * century);
        lowest_tilt = std::min(lowest_tilt, then.tilt);
        highest_tilt = std::max(highest_tilt, then.tilt);
        roundest = std::min(roundest, then.eccentricity);
        longest = std::max(longest, then.eccentricity);
        if ((then.tilt > 23.3) != above) {
            above = !above;
            ++crossings;
        }
    }
    // Earth's over the last million years: 22.1 to 24.5 degrees, and 0.00 to 0.06.
    CHECK(lowest_tilt > 21.5);
    CHECK(highest_tilt < 25.0);
    CHECK(highest_tilt - lowest_tilt > 1.5);
    CHECK(roundest >= 0.0);
    CHECK(longest < 0.07);
    CHECK(longest - roundest > 0.03);
    // A swing every 41,000 years or so crosses its middle about fifty times in a million.
    CHECK(crossings > 40);
    CHECK(crossings < 60);
}

TEST_CASE("the nearest point comes round against the equinoxes", "[planet][seasons]") {
    const System system;
    const double today = system.orbit.periapsis;
    CHECK_THAT(seasons_at(system, tilt, -10'500.0).periapsis,
               WithinAbs(std::fmod(today + 180.0, 360.0), 1e-9));
    CHECK_THAT(seasons_at(system, tilt, 21'000.0).periapsis, WithinAbs(today, 1e-9));

    System held = system;
    held.cycles.precession = 0.0;
    CHECK(seasons_at(held, tilt, -5'000.0).periapsis == today);
}

TEST_CASE("a day's sunlight follows the sun's height and the planet's distance",
          "[planet][seasons]") {
    const System system;
    const Seasons circle{.tilt = tilt, .eccentricity = 0.0, .periapsis = 0.0};
    const double flux =
        1'361.0 / std::pow(system.orbit.distance / hog::planet::astronomical_unit, 2.0);

    // At the equinox the sun crosses the equator overhead: over a day, 1/π of its light.
    CHECK_THAT(daily_sunlight(system, circle, 0.0, 0.0), WithinRel(flux / std::numbers::pi, 1e-9));
    // At midsummer the pole sees the sun all day at the height of the tilt.
    CHECK_THAT(daily_sunlight(system, circle, 90.0, 90.0),
               WithinRel(flux * std::sin(tilt * degree), 1e-9));
    CHECK(daily_sunlight(system, circle, -90.0, 90.0) == 0.0);
    CHECK(daily_sunlight(system, circle, 80.0, 270.0) == 0.0);
    // Midsummer at the pole outdoes the equator's.
    CHECK(daily_sunlight(system, circle, 90.0, 90.0) > daily_sunlight(system, circle, 0.0, 90.0));

    // Today the Sun is nearest in the northern winter.
    const Seasons today = seasons_at(system, tilt, 0.0);
    CHECK(daily_sunlight(system, today, 0.0, 270.0) > daily_sunlight(system, today, 0.0, 90.0));
}

TEST_CASE("summer sunlight in the far north swings enough to bring ice ages", "[planet][seasons]") {
    const System system;
    double weakest = 1.0e9;
    double strongest = 0.0;
    for (int step = 0; step < 250; ++step) {
        const Seasons then = seasons_at(system, tilt, -1'000.0 * step);
        const double sunlight = daily_sunlight(system, then, 65.0, 90.0);
        weakest = std::min(weakest, sunlight);
        strongest = std::max(strongest, sunlight);
    }
    // Earth's swung from about 440 to 550 over the last 250,000 years.
    CHECK(weakest > 400.0);
    CHECK(strongest < 580.0);
    CHECK(strongest - weakest > 60.0);
}

TEST_CASE("suns at the centre light the planet together, and a far one only as a point",
          "[planet][seasons]") {
    const Seasons circle{.tilt = tilt, .eccentricity = 0.0, .periapsis = 0.0};
    const System one;
    System two = one;
    two.stars.push_back(hog::planet::Star{});
    System far = one;
    far.stars.push_back(
        {.orbit = {.distance = 500.0 * hog::planet::astronomical_unit, .period = 10'000.0}});

    const double alone = daily_sunlight(one, circle, 30.0, 45.0);
    CHECK_THAT(daily_sunlight(two, circle, 30.0, 45.0), WithinRel(2.0 * alone, 1e-12));
    CHECK(daily_sunlight(far, circle, 30.0, 45.0) == alone);
}
