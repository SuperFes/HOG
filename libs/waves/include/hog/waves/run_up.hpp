#pragma once

namespace hog::waves {

// Waves running up a beach, as Stockdon and others fit it to ten beaches in 2006: the sea's mean
// level set up at the shore by the waves breaking, and the swash about it. Over a beach whose
// foreshore falls `slope` metres per metre, under waves of significant height `height` metres and
// peak period `period` s in deep water, each in metres.
struct RunUp {
    double setup = 0.0;
    // The swash's significant height: from the waves themselves, which runs up with each, and from
    // the slow surges of their groups.
    double incident = 0.0;
    double infragravity = 0.0;
};

// The slopes the fit was made over, which beaches are taken to lie within.
inline constexpr double gentlest_beach = 0.01;
inline constexpr double steepest_beach = 0.2;
// The groups' surges come round about this many times slower than the waves.
inline constexpr double surging = 6.0;

[[nodiscard]] RunUp run_up(double height, double period, double slope) noexcept;

// How high above the still sea two in a hundred of the waves run up.
[[nodiscard]] double run_up_reach(const RunUp& run) noexcept;

} // namespace hog::waves
