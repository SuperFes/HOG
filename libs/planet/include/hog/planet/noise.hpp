#pragma once

#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>

#include <algorithm>
#include <cstdint>

// Deterministic noise for world generation. Everything here is integer arithmetic on fixed-point
// values with 16 fractional bits, so results are bit-identical on every platform and compiler;
// convert to floating point only after generation.
namespace hog::planet {

inline constexpr int fixed_fraction_bits = 16;
inline constexpr std::int64_t fixed_one = std::int64_t{1} << fixed_fraction_bits;

[[nodiscard]] constexpr double fixed_to_double(std::int64_t fixed) noexcept {
    // Exact: dividing by a power of two.
    return static_cast<double>(fixed) / static_cast<double>(fixed_one);
}

// A point in noise space, each coordinate in fixed point. Lattice cells are one unit wide.
struct NoisePoint {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
};

// White noise: an independent 64-bit value per integer lattice point.
[[nodiscard]] constexpr std::uint64_t lattice_hash(std::uint64_t seed, std::int64_t x,
                                                   std::int64_t y, std::int64_t z) noexcept {
    return hash_values(seed, x, y, z);
}

// The same, as a fixed-point value in [0, 1).
[[nodiscard]] constexpr std::int64_t lattice_value(std::uint64_t seed, std::int64_t x,
                                                   std::int64_t y, std::int64_t z) noexcept {
    return static_cast<std::int64_t>(lattice_hash(seed, x, y, z) >> (64 - fixed_fraction_bits));
}

namespace detail {

// Interpolation weights carry extra precision; at 16 bits, their rounding error is amplified by
// the lerps into visible steps.
inline constexpr int weight_bits = 30;

// Perlin's quintic 6t^5 - 15t^4 + 10t^3 for t in [0, 1], from 16 fractional bits to 30. The
// intermediates stay below 2^63 because the result never exceeds 1.
[[nodiscard]] constexpr std::int64_t fade(std::int64_t t) noexcept {
    const std::int64_t t2 = t * t;                          // 32 bits
    const std::int64_t t3 = (t2 * t) >> (48 - weight_bits); // 30 bits
    const std::int64_t poly = ((6 * t2) - (15 * t * fixed_one) + (10 * fixed_one * fixed_one)) >>
                              (32 - weight_bits); // 30 bits
    return ((t3 * poly) + (std::int64_t{1} << (weight_bits - 1))) >> weight_bits;
}

[[nodiscard]] constexpr std::int64_t lerp(std::int64_t a, std::int64_t b,
                                          std::int64_t weight) noexcept {
    return a + ((((b - a) * weight) + (std::int64_t{1} << (weight_bits - 1))) >> weight_bits);
}

// Dot product with one of the twelve cube-edge gradients, picked as in Perlin's improved noise.
[[nodiscard]] constexpr std::int64_t gradient(std::uint64_t hash, std::int64_t x, std::int64_t y,
                                              std::int64_t z) noexcept {
    const auto h = static_cast<unsigned>(hash >> 60);
    const std::int64_t u = h < 8 ? x : y;
    std::int64_t v = z;
    if (h < 4) {
        v = y;
    } else if (h == 12 || h == 14) {
        v = x;
    }
    return ((h & 1U) != 0 ? -u : u) + ((h & 2U) != 0 ? -v : v);
}

} // namespace detail

// Classic Perlin gradient noise, in fixed point, clamped to [-1, 1] (it peaks just above 1).
// Zero at every lattice point.
[[nodiscard]] constexpr std::int64_t perlin(std::uint64_t seed, NoisePoint point) noexcept {
    // Arithmetic shifts and masks floor negative coordinates correctly.
    const std::int64_t cx = point.x >> fixed_fraction_bits;
    const std::int64_t cy = point.y >> fixed_fraction_bits;
    const std::int64_t cz = point.z >> fixed_fraction_bits;
    const std::int64_t fx = point.x & (fixed_one - 1);
    const std::int64_t fy = point.y & (fixed_one - 1);
    const std::int64_t fz = point.z & (fixed_one - 1);

    const auto corner = [&](std::int64_t dx, std::int64_t dy, std::int64_t dz) {
        return detail::gradient(lattice_hash(seed, cx + dx, cy + dy, cz + dz),
                                fx - (dx * fixed_one), fy - (dy * fixed_one),
                                fz - (dz * fixed_one));
    };

    const std::int64_t u = detail::fade(fx);
    const std::int64_t v = detail::fade(fy);
    const std::int64_t w = detail::fade(fz);
    const auto lerp = detail::lerp;
    const std::int64_t value = lerp(lerp(lerp(corner(0, 0, 0), corner(1, 0, 0), u),
                                         lerp(corner(0, 1, 0), corner(1, 1, 0), u), v),
                                    lerp(lerp(corner(0, 0, 1), corner(1, 0, 1), u),
                                         lerp(corner(0, 1, 1), corner(1, 1, 1), u), v),
                                    w);
    return std::clamp(value, -fixed_one, fixed_one);
}

inline constexpr int max_octaves = 16;

// Fractal sum of Perlin octaves, each at twice the frequency and half the amplitude of the last,
// normalised back to [-1, 1]. Every octave is seeded separately so they don't line up.
[[nodiscard]] constexpr std::int64_t fbm(std::uint64_t seed, NoisePoint point, int octaves) {
    HOG_ASSERT(octaves >= 1 && octaves <= max_octaves, "octave count out of range");
    std::int64_t sum = 0;
    std::int64_t amplitude = fixed_one;
    std::int64_t total_amplitude = 0;
    for (int octave = 0; octave < octaves; ++octave) {
        const auto octave_seed = hash_combine(seed, static_cast<std::uint64_t>(octave));
        sum += (perlin(octave_seed, point) * amplitude) >> fixed_fraction_bits;
        total_amplitude += amplitude;
        amplitude >>= 1;
        point = {.x = point.x * 2, .y = point.y * 2, .z = point.z * 2};
    }
    return (sum * fixed_one) / total_amplitude;
}

} // namespace hog::planet
