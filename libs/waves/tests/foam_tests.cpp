#include <hog/waves/foam.hpp>
#include <hog/waves/shoaling.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::waves::SeaState;
using hog::waves::Whitecaps;

namespace {

constexpr DVec3 east{1.0, 0.0, 0.0};
constexpr DVec3 north{0.0, 1.0, 0.0};

double white(const Whitecaps& caps) {
    return caps.breaking + caps.left;
}

} // namespace

TEST_CASE("the open sea whitens with the wind as Monahan measured", "[waves][foam]") {
    CHECK_THAT(white(hog::waves::whitecaps(10.0)), WithinRel(3.84e-6 * std::pow(10.0, 3.41), 1e-9));
    CHECK_THAT(white(hog::waves::whitecaps(20.0)), WithinRel(0.105, 0.005));
    // Most of what's white is foam left behind.
    const Whitecaps fresh = hog::waves::whitecaps(15.0);
    CHECK(fresh.breaking > 0.0);
    CHECK(fresh.left > fresh.breaking);
    CHECK(white(hog::waves::whitecaps(45.0)) == 1.0);
    CHECK(white(hog::waves::whitecaps(0.0)) == 0.0);
    CHECK(white(hog::waves::whitecaps(std::numeric_limits<double>::quiet_NaN())) == 0.0);
}

TEST_CASE("only the wind's own waves break white", "[waves][foam]") {
    constexpr double wind = 12.0;
    constexpr double g = hog::waves::gravity;
    const auto at_speed = [&](double over) {
        return hog::waves::breaking_share(g / (over * wind), wind);
    };
    CHECK(at_speed(2.0) == 0.0); // swell outrunning it
    CHECK(at_speed(1.0) == 1.0); // its own
    CHECK(at_speed(0.5) == 1.0);
    CHECK(at_speed(0.1) == 0.0); // ripples
    CHECK(hog::waves::breaking_share(1.0, 0.0) == 0.0);
    // A grown sea's peak breaks.
    CHECK(hog::waves::breaking_share(hog::waves::grown_sea(east * wind).peak, wind) == 1.0);
    for (int step = 0; step < 60; ++step) {
        const double frequency = 0.1 * std::pow(1.1, step);
        const double share = hog::waves::breaking_share(frequency, wind);
        CHECK(share >= 0.0);
        CHECK(share <= 1.0);
    }
}

TEST_CASE("a sea whitens as far as its waves have grown under the wind", "[waves][foam]") {
    const DVec3 wind = east * 14.0;
    const hog::waves::System grown = hog::waves::grown_sea(wind);
    const SeaState full{.systems = {grown}, .current = {}, .wind = wind};
    CHECK_THAT(white(hog::waves::whitecaps(full)),
               WithinRel(white(hog::waves::whitecaps(14.0)), 1e-9));

    // The same wind just begun to blow over a calm sea, or over another sea's swell.
    hog::waves::System young = grown;
    young.height /= 10.0;
    young.peak *= 3.0;
    young.peakedness = hog::waves::wind_sea_peakedness;
    const double begun =
        white(hog::waves::whitecaps(SeaState{.systems = {young}, .current = {}, .wind = wind}));
    CHECK(begun > 0.0);
    CHECK(begun < white(hog::waves::whitecaps(14.0)) / 2.0);
    const SeaState swell{.systems = {{.heading = north,
                                      .height = 3.0,
                                      .peak = 0.4,
                                      .peakedness = hog::waves::swell_peakedness,
                                      .spread = hog::waves::swell_spread}},
                         .current = {},
                         .wind = wind};
    CHECK(white(hog::waves::whitecaps(swell)) < white(hog::waves::whitecaps(14.0)) / 10.0);

    CHECK(white(hog::waves::whitecaps(SeaState{.systems = {grown}, .current = {}, .wind = {}})) ==
          0.0);
    CHECK(white(hog::waves::whitecaps(SeaState{.systems = {}, .current = {}, .wind = wind})) ==
          0.0);
}

TEST_CASE("how far above its mean a normal value goes so often", "[waves][foam]") {
    CHECK_THAT(hog::waves::exceeded(0.5), WithinAbs(0.0, 1e-9));
    CHECK_THAT(hog::waves::exceeded(0.158655), WithinAbs(1.0, 1e-5));
    CHECK_THAT(hog::waves::exceeded(0.0227501), WithinAbs(2.0, 1e-5));
    CHECK_THAT(hog::waves::exceeded(0.9), WithinAbs(-hog::waves::exceeded(0.1), 1e-9));
    CHECK(std::isfinite(hog::waves::exceeded(0.0)));
    CHECK(std::isfinite(hog::waves::exceeded(1.0)));
}

TEST_CASE("foam drifts with the current and the wind, streaking in a gale", "[waves][foam]") {
    const SeaState sea{.systems = {}, .current = north * 0.5, .wind = east * 10.0};
    CHECK(glm::length(hog::waves::foam_drift(sea) - DVec3{0.3, 0.5, 0.0}) < 1e-12);
    CHECK(hog::waves::streaking(5.0) == 1.0);
    CHECK(hog::waves::streaking(25.0) == 8.0);
    double before = 1.0;
    for (int step = 0; step < 60; ++step) {
        const double streaks = hog::waves::streaking(step * 0.5);
        CHECK(streaks >= before);
        before = streaks;
    }
}

TEST_CASE("waves break in the surf as they come to be too high for the depth", "[waves][foam]") {
    constexpr double height = 1.0;
    // Thornton and Guza's weighting at what the depth holds, and a little short of it.
    const double holds = height / hog::waves::breaking_depth;
    CHECK_THAT(hog::waves::breaking_waves(height, holds), WithinRel(1.0 - std::exp(-1.0), 1e-12));
    CHECK_THAT(hog::waves::breaking_waves(height, 2.0 * holds),
               WithinRel(std::pow(0.5, 4.0) * (1.0 - std::exp(-0.25)), 1e-12));
    // No more break where it's shallower still: there they're no higher than it holds.
    CHECK(hog::waves::breaking_waves(height, holds / 3.0) ==
          hog::waves::breaking_waves(height, holds));
    CHECK(hog::waves::breaking_waves(height, 0.0) == hog::waves::breaking_waves(height, holds));
    CHECK(hog::waves::breaking_waves(height, 50.0) < 1e-6);
    CHECK(hog::waves::breaking_waves(0.0, 1.0) == 0.0);
    double before = 0.0;
    for (int step = 40; step >= 0; --step) {
        const double breaking = hog::waves::breaking_waves(height, step * 0.5);
        CHECK(breaking >= before);
        before = breaking;
    }
    // As much of the surf is white as breaks, most of it foam left.
    const Whitecaps surf = hog::waves::surf(height, 1.5 * holds);
    CHECK_THAT(white(surf), WithinRel(hog::waves::breaking_waves(height, 1.5 * holds), 1e-12));
    CHECK(surf.left > surf.breaking);
    CHECK(white(hog::waves::surf(0.0, 1.0)) == 0.0);
}
