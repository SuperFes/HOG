#include <hog/core/hash.hpp>
#include <hog/globe/maths.hpp>
#include <hog/globe/noise.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace hog::globe {
namespace {

constexpr std::size_t gradient_count = 256;

// A spherical Fibonacci lattice: even over the sphere, with no axis of its own.
const std::array<DVec3, gradient_count>& gradients() {
    static const std::array<DVec3, gradient_count> table = [] {
        std::array<DVec3, gradient_count> spread{};
        const double golden = std::numbers::pi * (4.0 - (2.0 * std::numbers::phi));
        for (std::size_t k = 0; k < gradient_count; ++k) {
            const double z = 1.0 - (((2.0 * static_cast<double>(k)) + 1.0) / gradient_count);
            const double ring = std::sqrt(1.0 - (z * z));
            const double turn = golden * static_cast<double>(k);
            spread.at(k) = {ring * maths::cos(turn), ring * maths::sin(turn), z};
        }
        return spread;
    }();
    return table;
}

const DVec3& gradient_at(std::int64_t x, std::int64_t y, std::int64_t z, std::uint64_t seed) {
    return gradients()[hash_values(seed, x, y, z) % gradient_count];
}

// Quintic, so the field's second derivative is continuous across cells too.
double fade(double t) {
    return t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0);
}

double fade_slope(double t) {
    return 30.0 * t * t * ((t * (t - 2.0)) + 1.0);
}

// Unit-gradient lattice noise peaks just under 1 in 3D; this brings it near ±1.
constexpr double reach = 1.4;

} // namespace

Noise gradient_noise(DVec3 point, std::uint64_t seed) noexcept {
    const DVec3 cell = glm::floor(point);
    const DVec3 f = point - cell;
    const auto x = static_cast<std::int64_t>(cell.x);
    const auto y = static_cast<std::int64_t>(cell.y);
    const auto z = static_cast<std::int64_t>(cell.z);

    const DVec3& ga = gradient_at(x, y, z, seed);
    const DVec3& gb = gradient_at(x + 1, y, z, seed);
    const DVec3& gc = gradient_at(x, y + 1, z, seed);
    const DVec3& gd = gradient_at(x + 1, y + 1, z, seed);
    const DVec3& ge = gradient_at(x, y, z + 1, seed);
    const DVec3& gf = gradient_at(x + 1, y, z + 1, seed);
    const DVec3& gg = gradient_at(x, y + 1, z + 1, seed);
    const DVec3& gh = gradient_at(x + 1, y + 1, z + 1, seed);

    const double va = glm::dot(ga, f);
    const double vb = glm::dot(gb, f - DVec3{1.0, 0.0, 0.0});
    const double vc = glm::dot(gc, f - DVec3{0.0, 1.0, 0.0});
    const double vd = glm::dot(gd, f - DVec3{1.0, 1.0, 0.0});
    const double ve = glm::dot(ge, f - DVec3{0.0, 0.0, 1.0});
    const double vf = glm::dot(gf, f - DVec3{1.0, 0.0, 1.0});
    const double vg = glm::dot(gg, f - DVec3{0.0, 1.0, 1.0});
    const double vh = glm::dot(gh, f - DVec3{1.0, 1.0, 1.0});

    const DVec3 u{fade(f.x), fade(f.y), fade(f.z)};
    const DVec3 du{fade_slope(f.x), fade_slope(f.y), fade_slope(f.z)};

    // The trilinear blend of the corners' ramps, written as a polynomial in u.
    const double k1 = vb - va;
    const double k2 = vc - va;
    const double k3 = ve - va;
    const double k4 = va - vb - vc + vd;
    const double k5 = va - vc - ve + vg;
    const double k6 = va - vb - ve + vf;
    const double k7 = -va + vb + vc - vd + ve - vf - vg + vh;
    const double value = va + (u.x * k1) + (u.y * k2) + (u.z * k3) + (u.x * u.y * k4) +
                         (u.y * u.z * k5) + (u.z * u.x * k6) + (u.x * u.y * u.z * k7);

    const DVec3 blended = ga + (u.x * (gb - ga)) + (u.y * (gc - ga)) + (u.z * (ge - ga)) +
                          (u.x * u.y * (ga - gb - gc + gd)) + (u.y * u.z * (ga - gc - ge + gg)) +
                          (u.z * u.x * (ga - gb - ge + gf)) +
                          (u.x * u.y * u.z * (-ga + gb + gc - gd + ge - gf - gg + gh));
    const DVec3 ramp{k1 + (u.y * k4) + (u.z * k6) + (u.y * u.z * k7),
                     k2 + (u.z * k5) + (u.x * k4) + (u.z * u.x * k7),
                     k3 + (u.x * k6) + (u.y * k5) + (u.x * u.y * k7)};
    return {.value = reach * value, .gradient = reach * (blended + (du * ramp))};
}

Noise fbm(DVec3 point, const Octaves& octaves, double lod, std::uint64_t seed) noexcept {
    // Octaves whose wavelength is at least twice the detail asked for, and the next faded.
    const double shortest = std::max({2.0 * lod, octaves.finest, 1e-9});
    const double count = (maths::log(octaves.wavelength / shortest) / std::numbers::ln2) + 1.0;
    // A fixed rotation between octaves (orthonormal, no axis left in place).
    const glm::dmat3 turn{0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64};
    Noise sum;
    glm::dmat3 frame{1.0};
    double wavelength = octaves.wavelength;
    double amplitude = 1.0;
    for (int octave = 0; octave < 64 && static_cast<double>(octave) < count; ++octave) {
        const double weight = std::min(1.0, count - static_cast<double>(octave));
        const Noise layer = gradient_noise(frame * point / wavelength, hash_values(seed, octave));
        sum.value += amplitude * weight * layer.value;
        // Back from the turned, scaled lattice to the point's own frame.
        sum.gradient +=
            (amplitude * weight / wavelength) * (glm::transpose(frame) * layer.gradient);
        frame = turn * frame;
        wavelength *= 0.5;
        amplitude *= octaves.gain;
    }
    return sum;
}

} // namespace hog::globe
