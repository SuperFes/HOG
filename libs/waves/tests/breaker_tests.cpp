#include <hog/waves/breaker.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace hog::waves;
using Catch::Approx;

namespace {

// How far back the water's position along the wave falls as the phase climbs: the least of its
// slope over the phase against where it would run without the curl, for a wave of steepness
// `slope` (amplitude times wavenumber) and unit amplitude.
double most_overhang(double stage, double slope) {
    constexpr int steps = 4000;
    double before = 0.0;
    double least = 1.0;
    for (int i = 0; i <= steps; ++i) {
        const double phase = -std::numbers::pi + (2.0 * std::numbers::pi * i / steps);
        const Curl curl = curl_at(phase, stage, 1.0, slope);
        // The phase's own run, the usual choppiness pulling in the crest, and the curl.
        const double along = (phase / slope) - std::sin(phase) + curl.along;
        if (i > 0) {
            least = std::min(least, (along - before) / (2.0 * std::numbers::pi / steps) * slope);
        }
        before = along;
    }
    return least;
}

} // namespace

TEST_CASE("a wave not breaking has no curl", "[waves][breaker]") {
    for (const double phase : {-2.0, 0.0, 0.3, 1.0}) {
        const Curl curl = curl_at(phase, 0.0, 1.5, 0.2);
        CHECK(curl.along == 0.0);
        CHECK(curl.up == 0.0);
    }
    CHECK(curl_at(0.2, 1.0, 0.0, 0.2).along == 0.0);
}

TEST_CASE("the curl draws the front face back and drops the lip's tip, and leaves the back alone",
          "[waves][breaker]") {
    const Curl face = curl_at(1.0, 1.0, 1.0, 0.2);
    CHECK(face.along < -3.0);
    CHECK(curl_at(0.55, 1.0, 1.0, 0.2).up < -0.5);
    CHECK(curl_at(-1.0, 1.0, 1.0, 0.2).along == 0.0);
    CHECK(curl_at(-1.0, 1.0, 1.0, 0.2).up == Approx(0.0).margin(1e-4));
    // Gone at the foot, so the next wave is as it was.
    CHECK(curl_at(std::numbers::pi, 1.0, 1.0, 0.2).along == Approx(0.0).margin(1e-9));
    // A longer wave is drawn back further.
    CHECK(std::abs(curl_at(1.0, 1.0, 1.0, 0.1).along) > std::abs(face.along));
}

TEST_CASE("a curl's rates are the slopes of its offsets", "[waves][breaker]") {
    constexpr double step = 1e-6;
    for (const double phase : {-0.3, 0.1, 0.4, 0.9, 2.0}) {
        const Curl low = curl_at(phase - step, 0.8, 1.2, 0.15);
        const Curl high = curl_at(phase + step, 0.8, 1.2, 0.15);
        const Curl at = curl_at(phase, 0.8, 1.2, 0.15);
        CHECK(at.along_rate == Approx((high.along - low.along) / (2.0 * step)).margin(1e-4));
        CHECK(at.up_rate == Approx((high.up - low.up) / (2.0 * step)).margin(1e-5));
    }
}

TEST_CASE("a wave overhangs once it curls, at any steepness, and doesn't before",
          "[waves][breaker]") {
    for (const double slope : {0.09, 0.2, 0.45}) {
        // At these steepnesses the usual displacement alone never folds the surface.
        CHECK(most_overhang(0.0, slope) > 0.0);
        CHECK(most_overhang(1.0, slope) < 0.0);
        CHECK(most_overhang(0.3, slope) > most_overhang(1.0, slope));
    }
}

TEST_CASE("waves break once as high as the depth holds, and curl fully by half as high again",
          "[waves][breaker]") {
    CHECK(breaking_stage(0.2, 1.0) == 0.0);
    CHECK(breaking_stage(0.42, 1.0) == Approx(0.0).margin(1e-9));
    CHECK(breaking_stage(0.42 * 1.3, 1.0) == Approx(0.5));
    CHECK(breaking_stage(2.0, 1.0) == 1.0);
    CHECK(breaking_stage(1.0, 0.0) == 0.0);
}

TEST_CASE("moderate beaches plunge waves, gentle ones spill them and steep ones surge",
          "[waves][breaker]") {
    // A 1:10 beach under a 2% steep swell has an Iribarren number near 0.7.
    CHECK(plunging(0.1, 0.02) > 0.9);
    // 1:100 shelves spill them.
    CHECK(plunging(0.01, 0.02) < 0.05);
    // A cliff face surges them.
    CHECK(plunging(1.0, 0.01) < 0.05);
    CHECK(plunging(0.0, 0.02) == 0.0);
}
