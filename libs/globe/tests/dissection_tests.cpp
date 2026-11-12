#include "../src/dissection.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::globe::dissect;
using hog::globe::Dissected;
using hog::globe::Gullies;
using hog::globe::Undissected;

namespace {

constexpr double radius = 1e6;
constexpr Gullies gullies{.longest = 128.0, .octaves = 5, .seed = 7};

DVec3 at(double east, double north) {
    return glm::normalize(DVec3{1.0, east / radius, north / radius});
}

// Ground rising east `slope` in one, its walls standing up to `steepest`.
Undissected sloping(double east, double slope, double steepest) {
    return {.top = {.value = slope * east, .gradient = {0.0, slope, 0.0}},
            .fall = {0.0, slope, 0.0},
            .broad_fall = {0.0, slope, 0.0},
            .above_floor = 1e30,
            .steepest = steepest,
            .ice = 0.0};
}

Dissected cut(double east, double north, Undissected ground, double lod = 0.5) {
    return dissect(at(east, north), radius, ground, gullies, lod);
}

double cut_by(double east, double north, double slope, double steepest) {
    return cut(east, north, sloping(east, slope, steepest)).top.value - (slope * east);
}

} // namespace

TEST_CASE("flat ground is left as it lay", "[globe][dissection]") {
    for (int east_at = 0; 0.0 + (east_at * 7.0) < 300.0; ++east_at) {
        const double east = 0.0 + (east_at * 7.0);
        const Dissected flat = cut(east, east * 0.3, sloping(east, 0.0, 1.73));
        CHECK(flat.top.value == 0.0);
        CHECK(glm::length(flat.top.gradient) == 0.0);
    }
}

TEST_CASE("a slope is cut by gullies to crests between them, no steeper than it stands",
          "[globe][dissection]") {
    constexpr double slope = 0.4;
    constexpr double steepest = 1.0;
    double lowest = 0.0;
    double highest = 0.0;
    double steepest_seen = 0.0;
    constexpr double step = 0.5;
    for (int east_at = 0; 0.0 + (east_at * 50.0) < 400.0; ++east_at) {
        const double east = 0.0 + (east_at * 50.0);
        for (int north_at = 0; 0.0 + (north_at * step) < 600.0; ++north_at) {
            const double north = 0.0 + (north_at * step);
            const double here = cut(east, north, sloping(east, slope, steepest)).top.value;
            const double across = cut(east, north + step, sloping(east, slope, steepest)).top.value;
            const double along =
                cut(east + step, north, sloping(east + step, slope, steepest)).top.value;
            const double rise = std::hypot((across - here) / step, (along - here) / step);
            steepest_seen = std::max(steepest_seen, rise);
            lowest = std::min(lowest, here - (slope * east));
            highest = std::max(highest, here - (slope * east));
        }
    }
    // The deepest of the widest, some cutting deeper than others, three quarters as deep as their
    // walls can stand across half their width, taking what's left of the steepest once the slope
    // down them has its share, less the rounding.
    CHECK(highest - lowest > 0.75 * 0.9 * (steepest - slope) * gullies.longest / 2.0 * 0.75);
    // Where the gullies about fade and branch, their depth changes too, steepening them a little.
    CHECK(steepest_seen < 1.4 * steepest);
}

TEST_CASE("gullies run down the slope, not across it", "[globe][dissection]") {
    // Along a gully the cut changes little, across one it goes from crest to floor and back.
    double along = 0.0;
    double across = 0.0;
    for (int start_at = 0; 0.0 + (start_at * 97.0) < 2'000.0; ++start_at) {
        const double start = 0.0 + (start_at * 97.0);
        for (int by_at = 0; 0.0 + (by_at * 1.0) < 40.0; ++by_at) {
            const double by = 0.0 + (by_at * 1.0);
            along += std::abs(cut_by(start + by + 1.0, start, 0.4, 1.0) -
                              cut_by(start + by, start, 0.4, 1.0));
            across += std::abs(cut_by(start, start + by + 1.0, 0.4, 1.0) -
                               cut_by(start, start + by, 0.4, 1.0));
        }
    }
    CHECK(across > 3.0 * along);
}

TEST_CASE("gullies keep the ground as high as it stood, on the whole", "[globe][dissection]") {
    double total = 0.0;
    double deepest = 0.0;
    int count = 0;
    for (int east_at = 0; 0.0 + (east_at * 13.7) < 3'000.0; ++east_at) {
        const double east = 0.0 + (east_at * 13.7);
        for (int north_at = 0; 0.0 + (north_at * 17.3) < 3'000.0; ++north_at) {
            const double north = 0.0 + (north_at * 17.3);
            const double by = cut_by(east, north, 0.4, 1.0);
            total += by;
            deepest = std::max(deepest, std::abs(by));
            ++count;
        }
    }
    CHECK(std::abs(total / count) < 0.1 * deepest);
}

TEST_CASE("what the gullies take off the ground is told", "[globe][dissection]") {
    for (int east_at = 0; east_at < 60; ++east_at) {
        const double east = east_at * 7.3;
        const Undissected ground = sloping(east, 0.5, 1.73);
        const Dissected cut_down = cut(east, east * 1.7, ground);
        CHECK_THAT(cut_down.cut, WithinAbs(ground.top.value - cut_down.top.value, 1e-9));
    }
}

TEST_CASE("no gully cuts below the floor its ground drains to", "[globe][dissection]") {
    constexpr double room = 6.0;
    for (int east_at = 0; 0.0 + (east_at * 3.1) < 500.0; ++east_at) {
        const double east = 0.0 + (east_at * 3.1);
        for (int north_at = 0; 0.0 + (north_at * 2.3) < 500.0; ++north_at) {
            const double north = 0.0 + (north_at * 2.3);
            Undissected ground = sloping(east, 0.6, 1.73);
            ground.above_floor = room;
            CHECK(cut(east, north, ground).top.value >= ground.top.value - room - 1e-9);
        }
    }
}

TEST_CASE("gullies narrower than a sample is wide are left out", "[globe][dissection]") {
    for (int east_at = 0; 0.0 + (east_at * 11.0) < 500.0; ++east_at) {
        const double east = 0.0 + (east_at * 11.0);
        const Undissected ground = sloping(east, 0.5, 1.73);
        CHECK(cut(east, 3.0 * east, ground, gullies.longest).top.value == ground.top.value);
        // Coarser samples keep only the wider gullies, so they differ from fine ones by less.
        const double fine = cut(east, 3.0 * east, ground, 0.5).top.value;
        const double coarse = cut(east, 3.0 * east, ground, gullies.longest / 4.0).top.value;
        CHECK(std::abs(fine - coarse) <= 0.6 * gullies.longest);
    }
}

TEST_CASE("ice fills gullies, as deep as it lay, its crests standing out where it's thin",
          "[globe][dissection]") {
    SECTION("deep ice covers them, its top as it lay") {
        for (int north_at = 0; 0.0 + (north_at * 3.7) < 500.0; ++north_at) {
            const double north = 0.0 + (north_at * 3.7);
            Undissected ground = sloping(100.0, 0.5, 1.73);
            ground.ice = 200.0;
            const Dissected iced = cut(100.0, north, ground);
            CHECK_THAT(iced.top.value, WithinAbs(ground.top.value, 1e-9));
            CHECK(iced.ice >= 0.0);
        }
    }
    SECTION("thin ice lies in the gullies, crests bare, about as much of it as lay") {
        double ice = 0.0;
        int bare = 0;
        int count = 0;
        for (int east_at = 0; 0.0 + (east_at * 9.1) < 1'000.0; ++east_at) {
            const double east = 0.0 + (east_at * 9.1);
            for (int north_at = 0; 0.0 + (north_at * 7.3) < 1'000.0; ++north_at) {
                const double north = 0.0 + (north_at * 7.3);
                Undissected ground = sloping(east, 0.5, 1.73);
                ground.ice = 5.0;
                const Dissected iced = cut(east, north, ground);
                Undissected bed = ground;
                bed.ice = 0.0;
                bed.top.value -= ground.ice;
                CHECK(iced.top.value >= cut(east, north, bed).top.value - 1e-9);
                ice += iced.ice;
                bare += iced.ice == 0.0 ? 1 : 0;
                ++count;
            }
        }
        CHECK(bare > count / 10);
        CHECK_THAT(ice / count, WithinAbs(5.0, 2.0));
    }
}

TEST_CASE("the slope through the gullies is told", "[globe][dissection]") {
    constexpr double h = 0.05;
    const auto top = [](double east, double north) {
        return cut(east, north, sloping(east, 0.4, 1.0)).top.value;
    };
    double worst = 0.0;
    double total = 0.0;
    int count = 0;
    for (int east_at = 0; east_at < 16; ++east_at) {
        const double east = east_at * 37.1;
        for (int north_at = 0; north_at < 400; ++north_at) {
            const double north = north_at * 1.3;
            const DVec3 told = cut(east, north, sloping(east, 0.4, 1.0)).top.gradient;
            const double rising_east = (top(east + h, north) - top(east - h, north)) / (2.0 * h);
            const double rising_north = (top(east, north + h) - top(east, north - h)) / (2.0 * h);
            const double error = std::hypot(told.y - rising_east, told.z - rising_north);
            worst = std::max(worst, error);
            total += error;
            ++count;
        }
    }
    UNSCOPED_INFO("told slope off by " << total / count << " on average, " << worst << " at worst");
    CHECK(total / count < 0.06);
    WARN("told worst " << worst << " mean " << total / count);
}

TEST_CASE("gullies widely spaced on a gentle slope stand no steeper than told",
          "[globe][dissection]") {
    // Wide gullies cut deep enough for their walls to hold them back, the finer ones cut into
    // their walls: where they branch the stripes run close, and none may swing about there.
    constexpr Gullies wide{.longest = 1024.0, .octaves = 7, .seed = 11};
    constexpr double lod = 16.0;
    constexpr double slope = 0.24;
    constexpr double steepest = 1.73;
    constexpr double h = 0.5;
    const auto top = [&](double east, double north) {
        return dissect(at(east, north), radius, sloping(east, slope, steepest), wide, lod).top;
    };
    double worst = 0.0;
    double steepest_seen = 0.0;
    double best_e = 0.0;
    double best_n = 0.0;
    for (int east_at = 0; east_at < 150; ++east_at) {
        const double east = east_at * 10.0;
        for (int north_at = 0; north_at < 150; ++north_at) {
            const double north = north_at * 10.0;
            const DVec3 told = top(east, north).gradient;
            const double rising_east =
                (top(east + h, north).value - top(east - h, north).value) / (2.0 * h);
            const double rising_north =
                (top(east, north + h).value - top(east, north - h).value) / (2.0 * h);
            worst = std::max(worst, std::hypot(told.y - rising_east, told.z - rising_north));
            if (std::hypot(rising_east, rising_north) > steepest_seen) {
                best_e = east;
                best_n = north;
            }
            steepest_seen = std::max(steepest_seen, std::hypot(rising_east, rising_north));
        }
    }
    WARN("wide worst " << worst << " steepest " << steepest_seen << " at " << best_e << ","
                       << best_n);
}

TEST_CASE("the bed's heights stand about within the spread told, which is told as it changes, "
          "as the cut is",
          "[globe][dissection]") {
    constexpr double h = 0.05;
    int spread = 0;
    int within = 0;
    double share = 0.0;
    double spread_off = 0.0;
    double cut_off = 0.0;
    int count = 0;
    for (int east_at = 0; east_at < 16; ++east_at) {
        const double east = east_at * 37.1;
        for (int north_at = 0; north_at < 200; ++north_at) {
            const double north = north_at * 2.3;
            const auto made = [](double e, double n) { return cut(e, n, sloping(e, 0.4, 1.0)); };
            const Dissected here = made(east, north);
            within += std::abs(here.cut) <= 1.5 * here.spread.value ? 1 : 0;
            share += std::abs(here.cut) / here.spread.value;
            spread += here.spread.value > 0.0 ? 1 : 0;
            // How each changes a metre east and north, against how its gradient says it does along
            // the ground there.
            const DVec3 eastward =
                (at(east + h, north) - at(east - h, north)) * (radius / (2.0 * h));
            const DVec3 northward =
                (at(east, north + h) - at(east, north - h)) * (radius / (2.0 * h));
            const auto off = [&](DVec3 gradient, auto of) {
                const double de =
                    (of(made(east + h, north)) - of(made(east - h, north))) / (2.0 * h);
                const double dn =
                    (of(made(east, north + h)) - of(made(east, north - h))) / (2.0 * h);
                return std::hypot(glm::dot(gradient, eastward) - de,
                                  glm::dot(gradient, northward) - dn);
            };
            spread_off +=
                off(here.spread.gradient, [](const Dissected& d) { return d.spread.value; });
            cut_off += off(here.cut_gradient, [](const Dissected& d) { return d.cut; });
            ++count;
        }
    }
    CHECK(spread == count);
    UNSCOPED_INFO("within the spread at " << within << " of " << count << ", " << share / count
                                          << " of it on average; spread off by "
                                          << spread_off / count << ", cut by " << cut_off / count);
    // Most of the bed stands about its floors and crests; finer gullies cut a little past them
    // down the steeper walls of those before, now and then.
    CHECK(share / count > 0.4);
    CHECK(share / count < 0.9);
    CHECK(within > 99 * count / 100);
    // As near as differences over a twentieth of a metre tell, on gullies a hundred metres apart.
    CHECK(spread_off / count < 1e-6);
    CHECK(cut_off / count < 0.06);
}

TEST_CASE("what's laid settles into the bed's hollows first, level over it once as deep as they "
          "spread",
          "[globe][dissection]") {
    using hog::globe::settled;
    for (const double spread : {0.5, 2.0, 7.0}) {
        for (const double thick : {0.01, 0.1, 0.4, 1.0, 1.9, 2.0, 5.0, 9.0}) {
            const hog::globe::Settled level = settled(thick, spread);
            CHECK(level.level >= -spread);
            if (thick >= spread) {
                CHECK(level.level == thick);
            }
            // As much over the evenly spread bed as was laid.
            double laid = 0.0;
            constexpr int steps = 100'000;
            for (int step = 0; step < steps; ++step) {
                const double bed = spread * ((2.0 * (step + 0.5) / steps) - 1.0);
                laid += std::max(0.0, level.level - bed);
            }
            CHECK_THAT(laid / steps, WithinAbs(thick, 1e-4 * thick));
            // How it changes as each does, as told.
            constexpr double h = 1e-6;
            CHECK_THAT(level.per_thick, WithinAbs((settled(thick + h, spread).level -
                                                   settled(thick - h, spread).level) /
                                                      (2.0 * h),
                                                  1e-4));
            CHECK_THAT(level.per_spread, WithinAbs((settled(thick, spread + h).level -
                                                    settled(thick, spread - h).level) /
                                                       (2.0 * h),
                                                   1e-4));
        }
    }
    // Nothing laid lies nowhere, under the lowest of it.
    CHECK(settled(0.0, 3.0).level == -3.0);
}

TEST_CASE("gullies move the bed within their bounds anywhere in a ball", "[globe][dissection]") {
    std::mt19937_64 random{3};
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    double sampled = 0.0;
    double bounded = 0.0;
    for (int n = 0; n < 60; ++n) {
        const double steepest = 0.5 + unit(random);
        const double lod = n % 3 == 0 ? 0.5 : 8.0 * unit(random);
        const double east = 5'000.0 * unit(random);
        const double north = 5'000.0 * unit(random);
        const double within = 1.0 + (200.0 * unit(random) * unit(random));
        const hog::globe::Interval bounds =
            hog::globe::gully_bounds(at(east, north) * radius, within, steepest, gullies, lod);
        double low = 1e300;
        double high = -1e300;
        for (int k = 0; k < 300; ++k) {
            const double turn = 2.0 * std::numbers::pi * unit(random);
            const double off = within * std::sqrt(unit(random));
            const DVec3 point = at(east + (off * std::cos(turn)), north + (off * std::sin(turn)));
            // Any way down, no steeper than the walls stand, over any floor.
            const double slope = steepest * unit(random);
            const double way = 2.0 * std::numbers::pi * unit(random);
            const DVec3 fall = DVec3{0.0, std::cos(way), std::sin(way)} * slope;
            const Undissected ground{.top = {.value = 10.0, .gradient = fall},
                                     .fall = fall,
                                     .broad_fall = fall,
                                     .above_floor = 40.0 * unit(random),
                                     .steepest = steepest};
            const double moved = dissect(point, radius, ground, gullies, lod).top.value - 10.0;
            CHECK(bounds.contains(moved));
            low = std::min(low, moved);
            high = std::max(high, moved);
        }
        sampled += high - low;
        bounded += bounds.high - bounds.low;
    }
    WARN("sampled span " << sampled / bounded << " of the bounds'");
}
