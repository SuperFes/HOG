#include <hog/waves/shoaling.hpp>
#include <hog/waves/waves.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <ranges>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::waves::orbit_stretch;
using hog::waves::phase_lag;
using hog::waves::phase_lag_slope;
using hog::waves::shallow_wavenumber;
using hog::waves::shoaling;

namespace {

// The dispersion relation solved outright: the wavenumber k with k·tanh(k·depth) = deep.
double exact_wavenumber(double deep, double depth) {
    double k = std::max(deep, std::sqrt(deep / depth));
    for (int step = 0; step < 50; ++step) {
        const double t = std::tanh(k * depth);
        const double f = (k * t) - deep;
        const double df = t + (k * depth * (1.0 - (t * t)));
        k -= f / df;
    }
    return k;
}

// Metres deep the shore's floor lies `out` metres from the waterline.
double profile(double out) {
    return std::max(hog::waves::dean_scale * std::cbrt(out * out), out / hog::waves::shelf_run);
}

constexpr std::array deeps{0.01, 0.04, 0.2, 1.0}; // rad/m: swell of 20 s to chop of 2 s

} // namespace

TEST_CASE("shallow water shortens waves as the dispersion relation has it", "[waves][shoaling]") {
    for (const double deep : deeps) {
        for (const double depth : {0.05, 0.5, 2.0, 10.0, 40.0, 200.0}) {
            CAPTURE(deep, depth);
            CHECK_THAT(shallow_wavenumber(deep, depth),
                       WithinRel(exact_wavenumber(deep, depth), 2e-3));
        }
    }
    CHECK(shallow_wavenumber(0.04, 1'000.0) == 0.04);
}

TEST_CASE("waves first dip then rise as they shoal", "[waves][shoaling]") {
    CHECK(shoaling(0.04, 1'000.0) == 1.0);
    // The least, about 0.913, where k·h is about 1.2.
    double least = 2.0;
    double where = 0.0;
    for (int step = 1; step < 4'000; ++step) {
        const double depth = step * 0.05;
        if (const double found = shoaling(0.04, depth); found < least) {
            least = found;
            where = shallow_wavenumber(0.04, depth) * depth;
        }
    }
    CHECK_THAT(least, WithinAbs(0.913, 0.01));
    CHECK_THAT(where, WithinAbs(1.2, 0.15));
    // Green's law in the shallows: as the depth to the minus a quarter.
    CHECK_THAT(shoaling(0.04, 0.1) / shoaling(0.04, 1.6), WithinRel(2.0, 0.02));
    // And the orbit flattens to as much over k·h.
    CHECK(orbit_stretch(0.04, 1'000.0) == 1.0);
    CHECK_THAT(orbit_stretch(0.04, 0.2) * shallow_wavenumber(0.04, 0.2) * 0.2,
               WithinRel(1.0, 0.01));
}

TEST_CASE("a wave's phase lags as it comes in over the shore", "[waves][shoaling]") {
    for (const double deep : deeps) {
        // Straight along the profile, from far enough out that the wave doesn't feel the floor.
        const double far = hog::waves::deep_enough / deep * hog::waves::shelf_run;
        double lag = 0.0;
        constexpr double step = 0.05;
        double out = far;
        for (const double depth : {40.0, 15.0, 3.0, 0.5, 0.05}) {
            CAPTURE(deep, depth);
            while (profile(out - (step / 2.0)) > depth) {
                const double middle = out - (step / 2.0);
                lag += (shallow_wavenumber(deep, profile(middle)) - deep) * step;
                out -= step;
            }
            CHECK_THAT(phase_lag(deep, depth), WithinAbs(lag, 0.01 + (0.005 * lag)));
            // And it turns with the depth as its own slope has it, and as the wavenumber says.
            const double h = std::max(depth * 1e-3, 1e-4);
            const double slope = phase_lag_slope(deep, depth);
            CHECK_THAT(slope, WithinAbs((phase_lag(deep, depth + h) - phase_lag(deep, depth - h)) /
                                            (2.0 * h),
                                        1e-3 * (1.0 + std::abs(slope))));
            const double run = depth < hog::waves::shelf_depth
                                   ? 1.5 * std::sqrt(depth) / std::pow(hog::waves::dean_scale, 1.5)
                                   : hog::waves::shelf_run;
            CHECK_THAT(slope, WithinAbs(-(shallow_wavenumber(deep, depth) - deep) * run,
                                        0.01 * (1.0 + std::abs(slope))));
        }
    }
    CHECK(phase_lag(0.04, 1'000.0) == 0.0);
}

TEST_CASE("the phase lag's polynomials fall to nothing in deep enough water", "[waves][shoaling]") {
    const auto at_one = [](const auto& p) { return std::accumulate(p.begin(), p.end(), 0.0); };
    CHECK_THAT(at_one(hog::waves::shelf_lag), WithinAbs(0.0, 1e-9));
    CHECK_THAT(at_one(hog::waves::shoreface_lag), WithinAbs(0.0, 1e-9));
    CHECK(hog::waves::shoreface_lag.at(1) == 0.0);
    // Continuous where the shoreface gives way to the shelf.
    for (const double deep : deeps) {
        const double turn = hog::waves::shelf_depth;
        CAPTURE(deep);
        CHECK_THAT(phase_lag(deep, turn * (1.0 - 1e-9)),
                   WithinAbs(phase_lag(deep, turn * (1.0 + 1e-9)), 1e-6));
    }
}

TEST_CASE("waves too high for the depth break down to what it holds", "[waves][shoaling]") {
    CHECK(hog::waves::depth_limited(1.0, 10.0) == 1.0);
    CHECK_THAT(hog::waves::depth_limited(2.0, 1.0), WithinRel(0.21, 1e-12));
    CHECK(hog::waves::depth_limited(1.0, -2.0) == 0.0);
}
