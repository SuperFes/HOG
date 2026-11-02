#include <hog/waves/breaker.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace hog::waves;
using Catch::Approx;

namespace {

// How far a wave of steepness `slope` (amplitude times wavenumber) at `stage` leans back over
// itself: the most the water's position along the wave falls as the phase climbs the front face.
double most_overhang(double stage, double slope) {
    constexpr int steps = 2000;
    double before = 0.0;
    double least = 1.0;
    bool first = true;
    for (int i = 0; i <= steps; ++i) {
        const double phase = -std::numbers::pi + (2.0 * std::numbers::pi * i / steps);
        const Curl curl = curl_at(phase, stage, 1.0);
        // Along the wave in units of the amplitude: the phase's own run, the usual choppiness
        // pulling in the crest, and the curl.
        const double along = (phase / slope) - std::sin(phase) + curl.along;
        if (!first) {
            least = std::min(least, (along - before) / (2.0 * std::numbers::pi / steps));
        }
        before = along;
        first = false;
    }
    return least;
}

} // namespace

TEST_CASE("a wave not breaking has no curl", "[waves][breaker]") {
    for (const double phase : {-2.0, 0.0, 0.3, 1.0}) {
        const Curl curl = curl_at(phase, 0.0, 1.5);
        CHECK(curl.along == 0.0);
        CHECK(curl.up == 0.0);
    }
    CHECK(curl_at(0.2, 1.0, 0.0).along == 0.0);
}

TEST_CASE("the curl is pushed ahead over the crest and its tip drops lower down the face",
          "[waves][breaker]") {
    const Curl crest = curl_at(0.15, 1.0, 1.0);
    CHECK(crest.along > 3.0);
    const Curl face = curl_at(0.55, 1.0, 1.0);
    CHECK(face.up < -0.4);
    // Nothing behind the crest.
    CHECK(curl_at(-2.0, 1.0, 1.0).along < 1e-3);
    // Twice the height, twice the curl.
    CHECK(curl_at(0.15, 1.0, 2.0).along == Approx(2.0 * crest.along));
}

TEST_CASE("a curl's rates are the slopes of its offsets", "[waves][breaker]") {
    constexpr double step = 1e-6;
    for (const double phase : {-0.3, 0.1, 0.4, 0.9}) {
        const Curl low = curl_at(phase - step, 0.8, 1.2);
        const Curl high = curl_at(phase + step, 0.8, 1.2);
        const Curl at = curl_at(phase, 0.8, 1.2);
        CHECK(at.along_rate == Approx((high.along - low.along) / (2.0 * step)).margin(1e-5));
        CHECK(at.up_rate == Approx((high.up - low.up) / (2.0 * step)).margin(1e-5));
    }
}

TEST_CASE("a steep wave overhangs once it curls and doesn't before", "[waves][breaker]") {
    // At the steepness waves break at, the usual displacement alone never folds the surface.
    CHECK(most_overhang(0.0, 0.45) > 0.0);
    CHECK(most_overhang(0.3, 0.45) > most_overhang(1.0, 0.45));
    CHECK(most_overhang(1.0, 0.45) < 0.0);
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
