#include <hog/waves/tide.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

using hog::DVec3;
using hog::GameDuration;
using hog::GameTime;

namespace {

constexpr GameDuration day = std::chrono::hours{4};
// A coast on the equator: a unit direction.
constexpr DVec3 coast{0.6, 0.8, 0.0};

GameTime after(double days) {
    return GameTime{} + std::chrono::duration_cast<GameDuration>(day * days) +
           std::chrono::hours{24 * 365 * 56};
}

// The lowest and highest the tide stands at `up` over the day from `days`.
std::pair<double, double> range(DVec3 up, double days) {
    double low = 1e9;
    double high = -1e9;
    for (int step = 0; step < 200; ++step) {
        const double tide = hog::waves::tide_at(up, after(days + (step / 200.0)), day);
        low = std::min(low, tide);
        high = std::max(high, tide);
    }
    return {low, high};
}

double spread(std::pair<double, double> range) {
    return range.second - range.first;
}

} // namespace

TEST_CASE("the tide comes in and goes out twice a day", "[waves][tide]") {
    int rising = 0;
    double before = hog::waves::tide_at(coast, after(0.0), day);
    for (int step = 1; step <= 2'000; ++step) {
        const double tide = hog::waves::tide_at(coast, after(step / 200.0), day);
        rising += before < 0.0 && tide >= 0.0 ? 1 : 0;
        before = tide;
    }
    // Twice for each of the moon's days in ten of the planet's.
    CHECK(std::abs(rising - (2.0 * 10.0 / hog::waves::lunar_day)) <= 1.0);
}

TEST_CASE("the tide runs high at springs and low at neaps", "[waves][tide]") {
    // Over a whole round of them, about fifteen days apart.
    double springs = 0.0;
    double neaps = 1e9;
    for (int half_days = 0; half_days < 64; ++half_days) {
        const double daily = spread(range(coast, half_days / 2.0));
        springs = std::max(springs, daily);
        neaps = std::min(neaps, daily);
    }
    constexpr double ratio = (hog::waves::lunar_tide + hog::waves::solar_tide) /
                             (hog::waves::lunar_tide - hog::waves::solar_tide);
    CHECK(springs > neaps * ratio * 0.85);
    CHECK(springs < neaps * ratio * 1.15);
}

TEST_CASE("the tide falls away towards the poles and is never far from the mean", "[waves][tide]") {
    constexpr double most = 1.5 * (hog::waves::lunar_tide + hog::waves::solar_tide);
    for (int at = 0; at < 50; ++at) {
        const DVec3 up =
            glm::normalize(DVec3{std::cos(at * 1.3), std::sin(at * 0.7), (at % 7) - 3.0});
        for (int step = 0; step < 40; ++step) {
            CHECK(std::abs(hog::waves::tide_at(up, after(step * 0.37), day)) <= most);
        }
    }
    const DVec3 pole{0.0, 0.0, 1.0};
    CHECK(spread(range(pole, 2.0)) == 0.0);
    const DVec3 high = glm::normalize(DVec3{0.6, 0.8, 4.0});
    CHECK(spread(range(high, 2.0)) < 0.2 * spread(range(coast, 2.0)));
    CHECK(hog::waves::tide_at(coast, after(1.0), GameDuration{0}) == 0.0);
}

TEST_CASE("the tide changes smoothly along the coast", "[waves][tide]") {
    // A hundred metres along a planet of a few hundred kilometres' radius.
    constexpr double step = 100.0 / 300'000.0;
    const DVec3 along = glm::normalize(coast + DVec3{0.0, 0.0, step});
    for (int hour = 0; hour < 24; ++hour) {
        const GameTime time = after(hour / 24.0);
        CHECK(std::abs(hog::waves::tide_at(coast, time, day) -
                       hog::waves::tide_at(along, time, day)) < 0.005);
    }
}
