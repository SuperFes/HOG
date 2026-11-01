#include "terms.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/maths.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace hog::globe::terms {
namespace {

double fade(double t) {
    return t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0);
}

double fade_slope(double t) {
    return 30.0 * t * t * ((t * (t - 2.0)) + 1.0);
}

// One octave of gullies: stripes running straight down the slope so far, their phase drawn at
// each lattice corner and blended across the cell, so neighbouring gullies merge and branch.
Noise stripes(DVec3 point, DVec3 across, double wavelength, std::uint64_t seed) {
    const DVec3 scaled = point / wavelength;
    const DVec3 cell = glm::floor(scaled);
    const DVec3 f = scaled - cell;
    const DVec3 u{fade(f.x), fade(f.y), fade(f.z)};
    const DVec3 du{fade_slope(f.x), fade_slope(f.y), fade_slope(f.z)};
    const auto x = static_cast<std::int64_t>(cell.x);
    const auto y = static_cast<std::int64_t>(cell.y);
    const auto z = static_cast<std::int64_t>(cell.z);
    Noise sum;
    for (int corner = 0; corner < 8; ++corner) {
        const std::array<int, 3> at{corner & 1, (corner >> 1) & 1, (corner >> 2) & 1};
        DVec3 weights{0.0};
        DVec3 slopes{0.0};
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            const bool far = at.at(static_cast<std::size_t>(axis)) == 1;
            weights[axis] = far ? u[axis] : 1.0 - u[axis];
            slopes[axis] = far ? du[axis] : -du[axis];
        }
        const double weight = weights.x * weights.y * weights.z;
        const DVec3 weight_gradient{slopes.x * weights.y * weights.z,
                                    weights.x * slopes.y * weights.z,
                                    weights.x * weights.y * slopes.z};
        const std::uint64_t drawn = hash_values(seed, x + at[0], y + at[1], z + at[2]);
        const double phase = static_cast<double>(drawn >> 11U) * 0x1.0p-53;
        const DVec3 offset = f - DVec3{at[0], at[1], at[2]};
        const double turn = 2.0 * std::numbers::pi * (glm::dot(offset, across) + phase);
        const double wave = maths::cos(turn);
        sum.value += weight * wave;
        sum.gradient += (weight_gradient * wave) -
                        ((weight * maths::sin(turn) * 2.0 * std::numbers::pi) * across);
    }
    sum.gradient /= wavelength;
    return sum;
}

} // namespace

Noise gullies(const Context& context, DVec3 direction, DVec3 slope, double wavelength, int octave,
              double share, double weight) noexcept {
    const double steep = glm::length(slope);
    if (steep <= 1e-9 || weight <= 0.0) {
        return {};
    }
    const DVec3 across = glm::cross(direction, slope / steep);
    const Noise cut = stripes(direction * context.planet->radius, across, wavelength,
                              hash_values(context.seed(Term::Gullies), octave));
    const double depth = share * steep * wavelength * weight;
    return {.value = depth * cut.value, .gradient = depth * tangential(cut.gradient, direction)};
}

} // namespace hog::globe::terms
