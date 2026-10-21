#pragma once

#include <hog/core/math.hpp>
#include <hog/waves/waves.hpp>

namespace hog::waves {

// How much of the sea is white: where waves are breaking now, and the foam they leave behind,
// which thins away over a few seconds. Each a share of the surface.
struct Whitecaps {
    double breaking = 0.0;
    double left = 0.0;
};

// Over the open sea under a wind of `speed` m/s ten metres up, as Monahan and O'Muircheartaigh
// measured it: 3.84e-6 U^3.41, all of it white by 38 m/s.
[[nodiscard]] Whitecaps whitecaps(double speed) noexcept;

// How much a train of `frequency` rad/s takes part in breaking white under a wind of `speed`: the
// wind's own waves do, but not swell outrunning it, nor ripples too short to trap air.
[[nodiscard]] double breaking_share(double frequency, double speed) noexcept;

// As whitecaps(speed) for the sea's wind, but only as far as its waves have grown for it: by the
// mean square slope of the waves that break, against a sea grown full under that wind.
[[nodiscard]] Whitecaps whitecaps(const SeaState& sea);

// Of waves of root-mean-square height `height` metres running where it's `depth` deep, the share
// breaking as they pass, as Thornton and Guza weighted those too high for it: nothing in deep
// water, and 1 - 1/e once they're as high as it holds.
[[nodiscard]] double breaking_waves(double height, double depth) noexcept;

// How much of the sea is white in the surf there: of the waves breaking, their rollers, and the
// foam they leave.
[[nodiscard]] Whitecaps surf(double height, double depth) noexcept;

// How many standard deviations above its mean a normally distributed value exceeds `share` of the
// time; `share` is taken within (0, 1).
[[nodiscard]] double exceeded(double share) noexcept;

// How far foam drifts each second: with the current and the water the wind drags along.
[[nodiscard]] DVec3 foam_drift(const SeaState& sea) noexcept;

// How many times longer along the wind than across it the foam left lies: once a gale blows, in
// streaks.
[[nodiscard]] double streaking(double speed) noexcept;

} // namespace hog::waves
