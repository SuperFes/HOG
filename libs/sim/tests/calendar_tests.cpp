#include <hog/sim/calendar.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <numbers>

using namespace std::chrono;
using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::GameTime;
using hog::sim::Calendar;
using hog::sim::Season;

namespace {

constexpr double degree = std::numbers::pi / 180.0;

GameTime at(sys_days day, minutes time) {
    return hog::to_game_time(hog::WallClock::time_point{day + time});
}

DVec3 up_at(double latitude, double longitude) {
    return {std::cos(latitude * degree) * std::cos(longitude * degree),
            std::cos(latitude * degree) * std::sin(longitude * degree),
            std::sin(latitude * degree)};
}

// The share of one planet day the sun spends above the horizon at `up`.
double share_lit(const Calendar& in, GameTime start, DVec3 up) {
    constexpr int samples = 1'440;
    int lit = 0;
    for (int i = 0; i < samples; ++i) {
        if (in.sun_elevation(start + (in.day_length() * i / samples), up) > 0.0) {
            ++lit;
        }
    }
    return static_cast<double>(lit) / samples;
}

} // namespace

TEST_CASE("days follow the planet's day length from the Unix epoch", "[sim][calendar]") {
    const Calendar calendar{hours{4}, 23.44};
    CHECK(calendar.day(GameTime{}) == 0);
    CHECK(calendar.day(GameTime{hours{4}} - microseconds{1}) == 0);
    CHECK(calendar.day(GameTime{hours{4}}) == 1);
    CHECK(calendar.day(GameTime{} - microseconds{1}) == -1);
    CHECK(calendar.day(at(2026y / October / 11, 0min)) == 20'737 * 6);

    CHECK(calendar.time_of_day(at(2026y / October / 11, 2h)) == 0.5);
    CHECK(calendar.time_of_day(at(2026y / October / 11, 5h)) == 0.25);
    CHECK(calendar.time_of_day(GameTime{} - hours{1}) == 0.75);
    CHECK_THAT(calendar.local_time_of_day(GameTime{}, up_at(0.0, 90.0)), WithinAbs(0.25, 1e-12));
    CHECK_THAT(calendar.local_time_of_day(GameTime{}, up_at(45.0, -90.0)), WithinAbs(0.75, 1e-12));
}

TEST_CASE("seasons turn on Earth's real equinoxes and solstices", "[sim][calendar]") {
    struct Turn {
        GameTime time;
        Season before;
        Season after;
    };
    const auto turn =
        GENERATE(Turn{at(2024y / March / 20, 3h + 6min), Season::Winter, Season::Spring},
                 Turn{at(2026y / March / 20, 14h + 46min), Season::Winter, Season::Spring},
                 Turn{at(2026y / June / 21, 8h + 24min), Season::Spring, Season::Summer},
                 Turn{at(2026y / September / 23, 0h + 5min), Season::Summer, Season::Autumn},
                 Turn{at(2026y / December / 21, 20h + 50min), Season::Autumn, Season::Winter});
    const DVec3 north = up_at(42.0, 10.0);
    const DVec3 south = up_at(-42.0, 10.0);
    const auto opposite = [](Season season) {
        return static_cast<Season>((static_cast<int>(season) + 2) % 4);
    };

    CHECK(Calendar::season(turn.time - hours{1}, north) == turn.before);
    CHECK(Calendar::season(turn.time + hours{1}, north) == turn.after);
    CHECK(Calendar::season(turn.time - hours{1}, south) == opposite(turn.before));
    CHECK(Calendar::season(turn.time + hours{1}, south) == opposite(turn.after));
}

TEST_CASE("the sun stands over the tropic at the June solstice", "[sim][calendar]") {
    const Calendar calendar{hours{4}, 23.44};
    const DVec3 sun = calendar.sun_direction(at(2026y / June / 21, 8h + 24min));
    CHECK_THAT(glm::length(sun), WithinAbs(1.0, 1e-12));
    CHECK_THAT(sun.z, WithinAbs(std::sin(23.44 * degree), 1e-5));
}

TEST_CASE("noon and midnight on the equator at an equinox", "[sim][calendar]") {
    const Calendar calendar{hours{4}, 23.44};
    // 14:00 UTC is noon at longitude 0 with four-hour days, 46 minutes before the equinox.
    const GameTime noon = at(2026y / March / 20, 14h);
    const DVec3 up = up_at(0.0, 0.0);
    CHECK(calendar.sun_elevation(noon, up) > 89.9 * degree);
    CHECK(calendar.sun_elevation(noon - hours{2}, up) < -89.9 * degree);
    CHECK_THAT(calendar.sun_elevation(noon - hours{1}, up), WithinAbs(0.0, 0.01));
}

TEST_CASE("summer days are long and winter days short", "[sim][calendar]") {
    const Calendar calendar{hours{4}, 23.44};
    // Earth gives about 15 hours of 24 at 42 degrees north in June, and 9 in December.
    const DVec3 north = up_at(42.0, 0.0);
    const DVec3 south = up_at(-42.0, 0.0);
    const GameTime june = at(2026y / June / 21, 0min);
    const GameTime december = at(2026y / December / 21, 0min);
    CHECK_THAT(share_lit(calendar, june, north), WithinAbs(0.62, 0.02));
    CHECK_THAT(share_lit(calendar, december, north), WithinAbs(0.38, 0.02));
    CHECK_THAT(share_lit(calendar, june, south), WithinAbs(0.38, 0.02));

    const Calendar upright{hours{4}, 0.0};
    CHECK_THAT(share_lit(upright, june, north), WithinAbs(0.5, 0.01));
}

TEST_CASE("calendars reject bad input", "[sim][calendar]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    CHECK_THROWS_AS(Calendar(hours{0}, 23.44), AssertionFailed);
    CHECK_THROWS_AS(Calendar(hours{4}, -1.0), AssertionFailed);
    CHECK_THROWS_AS(Calendar(hours{4}, 91.0), AssertionFailed);
}
