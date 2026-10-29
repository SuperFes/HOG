#include <hog/client/tides.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

using namespace std::chrono;
using hog::DVec3;
using hog::GameTime;
using hog::client::tide_pulls;
using hog::planet::Ephemeris;
using hog::planet::System;

namespace {

constexpr hours planet_day{4};
// A coast at middling latitudes: a unit direction.
DVec3 coast() {
    return glm::normalize(DVec3{0.6, 0.8, 0.3});
}

GameTime after(double days) {
    return hog::to_game_time(hog::WallClock::time_point{sys_days{2026y / March / 1}}) +
           duration_cast<hog::GameDuration>(planet_day * days);
}

double tide(const Ephemeris& ephemeris, double days) {
    return hog::waves::tide_at(coast(), tide_pulls(ephemeris, after(days)));
}

// How far the tide rises and falls at the coast over the day from `days`.
double daily_range(const Ephemeris& ephemeris, double days) {
    double low = 1e9;
    double high = -1e9;
    for (int step = 0; step < 100; ++step) {
        const double now = tide(ephemeris, days + (step / 100.0));
        low = std::min(low, now);
        high = std::max(high, now);
    }
    return high - low;
}

} // namespace

TEST_CASE("the system's sun and moon draw the tide in twice a moon's day", "[client][tides]") {
    const Ephemeris ephemeris{System{}, planet_day, 23.44};
    const std::vector<hog::waves::Pull> pulls = tide_pulls(ephemeris, after(0.0));
    CHECK(pulls.size() == 2);

    int rising = 0;
    double before = tide(ephemeris, 0.0);
    for (int step = 1; step <= 2'000; ++step) {
        const double now = tide(ephemeris, step / 200.0);
        rising += before < 0.0 && now >= 0.0 ? 1 : 0;
        before = now;
    }
    // The moon falls behind the sun by a day a month, so its day is a little longer.
    const double moons_day = 1.0 / (1.0 - (1.0 / 29.53));
    CHECK(std::abs(rising - (2.0 * 10.0 / moons_day)) <= 1.0);
}

TEST_CASE("the system's tides run to springs and neaps twice a month", "[client][tides]") {
    const Ephemeris ephemeris{System{}, planet_day, 23.44};
    std::vector<double> ranges;
    ranges.reserve(70);
    for (int half_days = 0; half_days < 70; ++half_days) {
        ranges.push_back(daily_range(ephemeris, half_days / 2.0));
    }
    const auto [neaps, springs] = std::ranges::minmax(ranges);
    CHECK(springs > 1.8 * neaps);

    // The biggest ranges come about fifteen days apart.
    const auto first = std::ranges::max_element(ranges.begin(), ranges.begin() + 30);
    const auto second = std::ranges::max_element(first + 20, first + 40);
    CHECK(std::abs((static_cast<double>(std::distance(first, second)) / 2.0) - 14.77) <= 2.5);
}

TEST_CASE("a planet with no moons has only its suns' tides", "[client][tides]") {
    System moonless;
    moonless.moons.clear();
    const Ephemeris ephemeris{moonless, planet_day, 23.44};
    const std::vector<hog::waves::Pull> pulls = tide_pulls(ephemeris, after(0.0));
    REQUIRE(pulls.size() == 1);
    CHECK(daily_range(ephemeris, 3.0) < 2.0 * 1.5 * 1.25 * 0.29);
}
