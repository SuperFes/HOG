#include "../src/dissection.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
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
    // The widest cut as deep as their walls can stand across half their width, taking what's
    // left of the steepest once the slope down them has its share, less the rounding.
    CHECK(highest - lowest > 0.9 * (steepest - slope) * gullies.longest / 2.0 * 0.75);
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
