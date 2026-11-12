#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/interval.hpp>

#include <cstdint>

namespace hog::globe {

// A smooth field's value and its gradient at a point.
struct Noise {
    double value = 0.0;
    DVec3 gradient{0.0};
};

// Gradient noise on the unit integer lattice, within about ±1, its features about one unit
// across. Each lattice point's gradient is one of 256 spread evenly over the sphere, so no
// direction is favoured. Integer hashing and portable arithmetic only: the same bits everywhere.
[[nodiscard]] Noise gradient_noise(DVec3 point, std::uint64_t seed) noexcept;

// Octaves of gradient noise from `wavelength` down, each half the last and `gain` as strong,
// turned against the last so their lattices never line up. Octaves shorter than twice `lod`
// are left out, the last one faded in, so coarser samples are smooth versions of fine ones.
// Its amplitude is that of the first octave.
struct Octaves {
    double wavelength = 1.0;
    double gain = 0.5;
    double finest = 0.0; // the shortest wavelength ever summed
};

// Which octaves to sum: those no longer than `longest` and longer than `shorter_than`, so a sum
// can be taken a band at a time, coarse to fine, and come to the same.
struct Band {
    double longest = 1e300;
    double shorter_than = 0.0;
};

[[nodiscard]] Noise fbm(DVec3 point, const Octaves& octaves, double lod, std::uint64_t seed,
                        Band band = {}) noexcept;

// The steepest an fbm of `octaves` can be anywhere, per unit, at any lod: how far its value can
// change over a metre at most, so a value's distance from zero is at least the value over this.
[[nodiscard]] double fbm_steepest(const Octaves& octaves) noexcept;

// Bounds of the fbm fbm() gives anywhere within `within` metres of `middle`: octaves too fine for
// the ball by their reach, the rest by bounding each lattice cell the ball touches, no wider than
// the middle's value give or take the steepest it gets there.
[[nodiscard]] Interval fbm_bounds(DVec3 middle, double within, const Octaves& octaves, double lod,
                                  std::uint64_t seed, Band band = {}) noexcept;

} // namespace hog::globe
