#pragma once

#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// Deterministic noise for world generation. Everything here is integer arithmetic on fixed-point
// values, with 16 fractional bits unless asked for more, so results are bit-identical on every
// platform and compiler; convert to floating point only after generation.
namespace hog::planet {

inline constexpr int fixed_fraction_bits = 16;
// For fields whose steps would show, such as the ground's height.
inline constexpr int fine_fraction_bits = 24;

template <int Bits>
    requires(Bits >= fixed_fraction_bits && Bits <= fine_fraction_bits)
inline constexpr std::int64_t fixed_unit = std::int64_t{1} << Bits;

inline constexpr std::int64_t fixed_one = fixed_unit<fixed_fraction_bits>;

template <int Bits = fixed_fraction_bits>
[[nodiscard]] constexpr double fixed_to_double(std::int64_t fixed) noexcept {
    // Exact: dividing by a power of two.
    return static_cast<double>(fixed) / static_cast<double>(fixed_unit<Bits>);
}

// The cube root, rounded down: a bit of the root for every three of the value.
[[nodiscard]] constexpr std::uint64_t cube_root(std::uint64_t value) noexcept {
    std::uint64_t root = 0;
    for (int shift = 63; shift >= 0; shift -= 3) {
        root <<= 1U;
        const std::uint64_t grown = (3 * root * (root + 1)) + 1;
        if ((value >> shift) >= grown) {
            value -= grown << shift;
            ++root;
        }
    }
    return root;
}

// A point in noise space, each coordinate in fixed point with `Bits` fractional bits. Lattice
// cells are one unit wide.
template <int Bits = fixed_fraction_bits>
struct BasicNoisePoint {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
};

using NoisePoint = BasicNoisePoint<>;

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

// Perlin's quintic 6t^5 - 15t^4 + 10t^3 for t in [0, 1], from `Bits` fractional bits to 30. The
// intermediates stay below 2^63 because the result never exceeds 1, and t^3 is formed from t^2
// with as many low bits dropped as it takes to fit.
template <int Bits>
[[nodiscard]] constexpr std::int64_t fade(std::int64_t t) noexcept {
    constexpr std::int64_t one = fixed_unit<Bits>;
    constexpr int dropped = std::max(0, (3 * Bits) - 62);
    const std::int64_t t2 = t * t;
    const std::int64_t t3 = ((t2 >> dropped) * t) >> ((3 * Bits) - dropped - weight_bits);
    const std::int64_t poly =
        ((6 * t2) - (15 * t * one) + (10 * one * one)) >> ((2 * Bits) - weight_bits);
    return ((t3 * poly) + (std::int64_t{1} << (weight_bits - 1))) >> weight_bits;
}

// The fade's slope, 30t^2(1-t)^2, from `Bits` fractional bits to 30.
template <int Bits>
[[nodiscard]] constexpr std::int64_t fade_slope(std::int64_t t) noexcept {
    constexpr std::int64_t one = fixed_unit<Bits>;
    constexpr int squares = 24;
    const std::int64_t a = (t * t) >> ((2 * Bits) - squares);
    const std::int64_t b = ((one - t) * (one - t)) >> ((2 * Bits) - squares);
    constexpr int shift = (2 * squares) - weight_bits;
    return ((30 * a * b) + (std::int64_t{1} << (shift - 1))) >> shift;
}

// Fixed point times a weight, rounded back to the fixed point's precision.
[[nodiscard]] constexpr std::int64_t weigh(std::int64_t value, std::int64_t weight) noexcept {
    return ((value * weight) + (std::int64_t{1} << (weight_bits - 1))) >> weight_bits;
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
template <int Bits = fixed_fraction_bits>
[[nodiscard]] constexpr std::int64_t perlin(std::uint64_t seed,
                                            BasicNoisePoint<Bits> point) noexcept {
    constexpr std::int64_t one = fixed_unit<Bits>;
    // Arithmetic shifts and masks floor negative coordinates correctly.
    const std::int64_t cx = point.x >> Bits;
    const std::int64_t cy = point.y >> Bits;
    const std::int64_t cz = point.z >> Bits;
    const std::int64_t fx = point.x & (one - 1);
    const std::int64_t fy = point.y & (one - 1);
    const std::int64_t fz = point.z & (one - 1);

    const auto corner = [&](std::int64_t dx, std::int64_t dy, std::int64_t dz) {
        return detail::gradient(lattice_hash(seed, cx + dx, cy + dy, cz + dz), fx - (dx * one),
                                fy - (dy * one), fz - (dz * one));
    };

    const std::int64_t u = detail::fade<Bits>(fx);
    const std::int64_t v = detail::fade<Bits>(fy);
    const std::int64_t w = detail::fade<Bits>(fz);
    const auto lerp = detail::lerp;
    const std::int64_t value = lerp(lerp(lerp(corner(0, 0, 0), corner(1, 0, 0), u),
                                         lerp(corner(0, 1, 0), corner(1, 1, 0), u), v),
                                    lerp(lerp(corner(0, 0, 1), corner(1, 0, 1), u),
                                         lerp(corner(0, 1, 1), corner(1, 1, 1), u), v),
                                    w);
    return std::clamp(value, -one, one);
}

struct NoiseGradient {
    std::int64_t value = 0;
    // Partial derivatives per lattice unit, in fixed point.
    std::int64_t dx = 0;
    std::int64_t dy = 0;
    std::int64_t dz = 0;
};

// Perlin noise with its exact slope. The value is bit-identical to perlin().
template <int Bits = fixed_fraction_bits>
[[nodiscard]] constexpr NoiseGradient perlin_gradient(std::uint64_t seed,
                                                      BasicNoisePoint<Bits> point) noexcept {
    constexpr std::int64_t one = fixed_unit<Bits>;
    const std::int64_t cx = point.x >> Bits;
    const std::int64_t cy = point.y >> Bits;
    const std::int64_t cz = point.z >> Bits;
    const std::int64_t fx = point.x & (one - 1);
    const std::int64_t fy = point.y & (one - 1);
    const std::int64_t fz = point.z & (one - 1);

    // Corners in the order (0,0,0), (1,0,0), (0,1,0), (1,1,0), then the same at z = 1. The
    // dot product is linear, so each corner's gradient vector is its dot with the unit axes.
    std::array<std::int64_t, 8> value{};
    std::array<std::array<std::int64_t, 3>, 8> slope{};
    for (std::size_t corner = 0; corner < 8; ++corner) {
        const auto dx = static_cast<std::int64_t>(corner & 1U);
        const auto dy = static_cast<std::int64_t>((corner >> 1U) & 1U);
        const auto dz = static_cast<std::int64_t>(corner >> 2U);
        const std::uint64_t hash = lattice_hash(seed, cx + dx, cy + dy, cz + dz);
        value[corner] = detail::gradient(hash, fx - (dx * one), fy - (dy * one), fz - (dz * one));
        slope[corner] = {detail::gradient(hash, one, 0, 0), detail::gradient(hash, 0, one, 0),
                         detail::gradient(hash, 0, 0, one)};
    }

    const std::int64_t u = detail::fade<Bits>(fx);
    const std::int64_t v = detail::fade<Bits>(fy);
    const std::int64_t w = detail::fade<Bits>(fz);
    const auto lerp = detail::lerp;
    const std::int64_t blended =
        lerp(lerp(lerp(value[0], value[1], u), lerp(value[2], value[3], u), v),
             lerp(lerp(value[4], value[5], u), lerp(value[6], value[7], u), v), w);

    // The blend as a polynomial in the weights, k0 + k1 u + k2 v + k3 w + k4 uv + k5 vw + k6 wu
    // + k7 uvw, differentiated: the corner slopes blended the same way, plus each coefficient
    // times the fade's slope.
    const auto coefficients = [](const auto& c) {
        return std::array{c[0],
                          c[1] - c[0],
                          c[2] - c[0],
                          c[4] - c[0],
                          c[0] - c[1] - c[2] + c[3],
                          c[0] - c[2] - c[4] + c[6],
                          c[0] - c[1] - c[4] + c[5],
                          -c[0] + c[1] + c[2] - c[3] + c[4] - c[5] - c[6] + c[7]};
    };
    const auto weigh = detail::weigh;
    const std::int64_t uv = weigh(u, v);
    const std::int64_t vw = weigh(v, w);
    const std::int64_t wu = weigh(w, u);
    const std::int64_t uvw = weigh(uv, w);
    const auto blend = [&](const std::array<std::int64_t, 8>& k) {
        return k[0] + weigh(k[1], u) + weigh(k[2], v) + weigh(k[3], w) + weigh(k[4], uv) +
               weigh(k[5], vw) + weigh(k[6], wu) + weigh(k[7], uvw);
    };

    const auto k = coefficients(value);
    std::array<std::int64_t, 3> derivative{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        std::array<std::int64_t, 8> axis_slope{};
        for (std::size_t corner = 0; corner < 8; ++corner) {
            axis_slope[corner] = slope[corner][axis];
        }
        derivative[axis] = blend(coefficients(axis_slope));
    }
    derivative[0] += weigh(k[1] + weigh(k[4], v) + weigh(k[6], w) + weigh(k[7], vw),
                           detail::fade_slope<Bits>(fx));
    derivative[1] += weigh(k[2] + weigh(k[5], w) + weigh(k[4], u) + weigh(k[7], wu),
                           detail::fade_slope<Bits>(fy));
    derivative[2] += weigh(k[3] + weigh(k[6], u) + weigh(k[5], v) + weigh(k[7], uv),
                           detail::fade_slope<Bits>(fz));

    return {.value = std::clamp(blended, -one, one),
            .dx = derivative[0],
            .dy = derivative[1],
            .dz = derivative[2]};
}

inline constexpr int max_octaves = 16;

// Fractal sum of Perlin octaves, each at twice the frequency and half the amplitude of the last,
// normalised back to [-1, 1]. Every octave is seeded separately so they don't line up.
template <int Bits = fixed_fraction_bits>
[[nodiscard]] constexpr std::int64_t fbm(std::uint64_t seed, BasicNoisePoint<Bits> point,
                                         int octaves) {
    HOG_ASSERT(octaves >= 1 && octaves <= max_octaves, "octave count out of range");
    constexpr std::int64_t one = fixed_unit<Bits>;
    std::int64_t sum = 0;
    std::int64_t amplitude = one;
    std::int64_t total_amplitude = 0;
    for (int octave = 0; octave < octaves; ++octave) {
        const auto octave_seed = hash_combine(seed, static_cast<std::uint64_t>(octave));
        sum += (perlin(octave_seed, point) * amplitude) >> Bits;
        total_amplitude += amplitude;
        amplitude >>= 1;
        point = {.x = point.x * 2, .y = point.y * 2, .z = point.z * 2};
    }
    return (sum * one) / total_amplitude;
}

// Ridged fractal noise in [0, 1]: sharp crests where each octave crosses zero. Every octave is
// damped by the slope the octaves before it have built up, scaled by `erosion` (fixed point; zero
// turns it off), so steep flanks stay smooth while valleys and plateaus keep their detail.
template <int Bits = fixed_fraction_bits>
[[nodiscard]] constexpr std::int64_t ridges(std::uint64_t seed, BasicNoisePoint<Bits> point,
                                            int octaves, std::int64_t erosion = fixed_unit<Bits>) {
    HOG_ASSERT(octaves >= 1 && octaves <= max_octaves, "octave count out of range");
    HOG_ASSERT(erosion >= 0, "erosion must not be negative");
    constexpr std::int64_t one = fixed_unit<Bits>;
    // The slope and steepness can grow well past 1, so above 16 bits their products drop low bits
    // first to keep the headroom they have at 16.
    constexpr int dropped = (Bits - fixed_fraction_bits) / 2;
    constexpr int product_shift = Bits - (2 * dropped);
    std::int64_t sum = 0;
    std::int64_t amplitude = one;
    std::int64_t total_amplitude = 0;
    // Each octave's frequency doubles as its amplitude halves, so plain sums of the octaves' own
    // derivatives approximate the slope of the (unnormalised) sum.
    std::array<std::int64_t, 3> slope{};
    for (int octave = 0; octave < octaves; ++octave) {
        const auto octave_seed = hash_combine(seed, static_cast<std::uint64_t>(octave));
        const NoiseGradient noise = perlin_gradient(octave_seed, point);
        const std::int64_t crest = one - (noise.value < 0 ? -noise.value : noise.value);
        const std::int64_t ridge = (crest * crest) >> Bits;

        const std::array<std::int64_t, 3> cut{slope[0] >> dropped, slope[1] >> dropped,
                                              slope[2] >> dropped};
        const std::int64_t steepness =
            ((cut[0] * cut[0]) + (cut[1] * cut[1]) + (cut[2] * cut[2])) >> product_shift;
        const std::int64_t damping =
            one + (((erosion >> dropped) * (steepness >> dropped)) >> product_shift);
        sum += (((amplitude * ridge) >> Bits) * one) / damping;
        total_amplitude += amplitude;

        // The ridge's slope is -2 crest sign(n) dn, which flips at every crest and would make the
        // damping jump there. Dropping the sign keeps the magnitude and stays continuous.
        const std::int64_t scale = 2 * crest;
        slope[0] += (scale * noise.dx) >> Bits;
        slope[1] += (scale * noise.dy) >> Bits;
        slope[2] += (scale * noise.dz) >> Bits;

        amplitude >>= 1;
        point = {.x = point.x * 2, .y = point.y * 2, .z = point.z * 2};
    }
    return (sum * one) / total_amplitude;
}

} // namespace hog::planet
