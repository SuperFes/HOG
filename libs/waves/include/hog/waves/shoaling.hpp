#pragma once

#include <array>
#include <numbers>

namespace hog::waves {

// Waves running into shallow water over a sandy shore as planet::Elevation shapes its floor: Dean's
// profile, depth = A·x^(2/3) at x metres out, until a shelf falling a metre per shelf_run overtakes
// it. Change both together.
inline constexpr double dean_scale = 0.12; // A, m^(1/3)
inline constexpr double shelf_run = 100.0;
// Where the shelf overtakes the shoreface: metres deep.
inline constexpr double shelf_depth = shelf_run * shelf_run * dean_scale * dean_scale * dean_scale;

// Metres deep the shore's profile lies `out` metres from its waterline, and how steeply it deepens
// there, metres per metre.
[[nodiscard]] double profile_depth(double out) noexcept;
[[nodiscard]] double profile_slope(double out) noexcept;

// Past this many radians of a wave's deep-water wavenumber times the depth, it's taken not to feel
// the floor at all: there its length is within 1% of deep water's.
inline constexpr double deep_enough = std::numbers::pi;

// Waves break once their root-mean-square height reaches this share of the depth, as Thornton and
// Guza found on beaches.
inline constexpr double breaking_depth = 0.42;

// A wave's wavenumber, rad/m, where it's `depth` metres deep, from `deep` in deep water: Hunt's fit
// to the dispersion relation.
[[nodiscard]] double shallow_wavenumber(double deep, double depth) noexcept;

// How much higher a wave runs there than in deep water, as the energy it carries slows with it.
[[nodiscard]] double shoaling(double deep, double depth) noexcept;

// How far the water swings along the wave there, over how far it swings up and down: its orbit
// flattening into an ellipse as the floor nears.
[[nodiscard]] double orbit_stretch(double deep, double depth) noexcept;

// How many more radians a wave's phase has turned by the time it's come into water `depth` deep,
// over the shore's profile, than it would have running as far through deep water; and how that
// changes with the depth, per metre.
[[nodiscard]] double phase_lag(double deep, double depth) noexcept;
[[nodiscard]] double phase_lag_slope(double deep, double depth) noexcept;

// The phase lag over the shore's profile depends on the depth only through the functions F, over
// the shelf, and G, over the shoreface, of κ, the deep-water wavenumber times the depth: the
// integrals from κ to deep_enough of (kh/κ - 1) and of (kh/κ - 1)·√κ. Each is fit, within 1e-4,
// as a polynomial in t = √(κ/deep_enough) that's 0 at t = 1, G with no term in t, so its slope is
// the lag's own; shaders/waves.slang has the same.
inline constexpr std::array<double, 9> shelf_lag{1.2347241954,  -3.5420030563, 3.0825815818,
                                                 -0.1219981919, -2.1095339280, 4.6660317033,
                                                 -6.1531146267, 3.8080381810,  -0.8647258586};
inline constexpr std::array<double, 9> shoreface_lag{0.5626467669,  0.0,           -3.1638013226,
                                                     3.9954707819,  -2.2227549352, 3.3481431461,
                                                     -4.2511585803, 1.9068094313,  -0.1753552881};

// What of a wave's phase lag depends on its deep-water wavenumber alone: the κ at which the shelf
// gives way to the shoreface, how the shoreface's G scales into the lag, and what the lag is less
// that, inside the turn.
struct LagTerms {
    double turn = 0.0;
    double scale = 0.0;
    double offset = 0.0;
};
[[nodiscard]] LagTerms lag_terms(double deep) noexcept;

// What of waves of root-mean-square height `height`, metres, still runs where it's `depth` deep,
// once those too high for it have broken: 1 in deep enough water.
[[nodiscard]] double depth_limited(double height, double depth) noexcept;

} // namespace hog::waves
