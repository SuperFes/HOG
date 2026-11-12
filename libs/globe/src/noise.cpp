#include <hog/core/hash.hpp>
#include <hog/globe/maths.hpp>
#include <hog/globe/noise.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
// The steepest the noise gets, per lattice unit: measured at 3.16 over two million points, and
// some over that.
constexpr double steepest = 3.6;

// No corner's ramp reaches further than the cell's diagonal, and the noise is a blend of them.
const double farthest = reach * std::numbers::sqrt3;

// Bounds of a value blended from `from` to `to` by any share in `by`, within 0 to 1.
Interval blend(Interval from, Interval to, Interval by) noexcept {
    const auto at = [](double one, double other, double share) {
        return one + (share * (other - one));
    };
    return {.low = std::min(at(from.low, to.low, by.low), at(from.low, to.low, by.high)),
            .high = std::max(at(from.high, to.high, by.low), at(from.high, to.high, by.high))};
}

// Corners in the order x, then y, then z, blended by the shares along each.
Interval trilinear(const std::array<Interval, 8>& corners, const std::array<Interval, 3>& by) {
    const Interval below =
        blend(blend(corners[0], corners[1], by[0]), blend(corners[2], corners[3], by[0]), by[1]);
    const Interval above =
        blend(blend(corners[4], corners[5], by[0]), blend(corners[6], corners[7], by[0]), by[1]);
    return blend(below, above, by[2]);
}

// Bounds of `constant` + `slope` · f with f anywhere from `from` to `to`.
Interval linear(DVec3 slope, double constant, DVec3 from, DVec3 to) noexcept {
    Interval sum{.low = constant, .high = constant};
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        sum = sum + (slope[axis] * Interval{.low = from[axis], .high = to[axis]});
    }
    return sum;
}

// The fade's slope over `from` to `to`, which peaks at the cell's middle.
Interval fade_slopes(double from, double to) noexcept {
    const double ends_low = std::min(fade_slope(from), fade_slope(to));
    const double ends_high = std::max(fade_slope(from), fade_slope(to));
    return {.low = ends_low, .high = from <= 0.5 && to >= 0.5 ? fade_slope(0.5) : ends_high};
}

// Bounds of gradient noise and of its slope along each axis over a box in lattice units.
struct NoiseBounds {
    Interval value;
    std::array<Interval, 3> slope{};
    bool sloped = false; // whether `slope` holds; not over a box touching too many cells
};

// Over one cell at `cell`, of the box from `from` to `to` in it.
NoiseBounds cell_bounds(DVec3 cell, DVec3 from, DVec3 to, std::uint64_t seed) noexcept {
    std::array<DVec3, 8> toward{};
    std::array<DVec3, 8> offset{};
    std::array<Interval, 8> ramps{};
    for (std::size_t corner = 0; corner < 8; ++corner) {
        offset.at(corner) = {static_cast<double>(corner & 1U),
                             static_cast<double>((corner >> 1U) & 1U),
                             static_cast<double>((corner >> 2U) & 1U)};
        const DVec3 at = cell + offset.at(corner);
        toward.at(corner) =
            gradient_at(static_cast<std::int64_t>(at.x), static_cast<std::int64_t>(at.y),
                        static_cast<std::int64_t>(at.z), seed);
        ramps.at(corner) =
            linear(toward.at(corner), -glm::dot(toward.at(corner), offset.at(corner)), from, to);
    }
    const std::array<Interval, 3> shares{Interval{.low = fade(from.x), .high = fade(to.x)},
                                         Interval{.low = fade(from.y), .high = fade(to.y)},
                                         Interval{.low = fade(from.z), .high = fade(to.z)}};
    NoiseBounds bounds{.value = reach * trilinear(ramps, shares), .sloped = true};
    // One corner's ramp less another's, as one ramp, so what they share cancels.
    const auto rise = [&](std::size_t high, std::size_t low) {
        return linear(toward.at(high) - toward.at(low),
                      glm::dot(toward.at(low), offset.at(low)) -
                          glm::dot(toward.at(high), offset.at(high)),
                      from, to);
    };
    // Along each axis: the pairs of corners across it, and the shares along the other two.
    constexpr std::array<std::array<std::size_t, 8>, 3> pairs{
        {{1, 0, 3, 2, 5, 4, 7, 6}, {2, 0, 3, 1, 6, 4, 7, 5}, {4, 0, 5, 1, 6, 2, 7, 3}}};
    constexpr std::array<std::array<std::size_t, 2>, 3> others{{{1, 2}, {0, 2}, {0, 1}}};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto& pair = pairs.at(axis);
        const Interval first = shares.at(others.at(axis)[0]);
        const Interval second = shares.at(others.at(axis)[1]);
        const Interval ramp =
            blend(blend(rise(pair[0], pair[1]), rise(pair[2], pair[3]), first),
                  blend(rise(pair[4], pair[5]), rise(pair[6], pair[7]), first), second);
        std::array<Interval, 8> along{};
        for (std::size_t corner = 0; corner < 8; ++corner) {
            const double component = toward.at(corner)[static_cast<glm::length_t>(axis)];
            along.at(corner) = {.low = component, .high = component};
        }
        bounds.slope.at(axis) =
            reach * (trilinear(along, shares) + (fade_slopes(from[static_cast<glm::length_t>(axis)],
                                                             to[static_cast<glm::length_t>(axis)]) *
                                                 ramp));
    }
    return bounds;
}

// Over the box from `low` to `high`, from those over each cell it touches; a box touching too
// many is bounded by the noise's reach.
NoiseBounds noise_bounds(DVec3 low, DVec3 high, std::uint64_t seed) noexcept {
    const DVec3 first = glm::floor(low);
    const DVec3 last = glm::floor(high);
    const DVec3 cells = last - first + DVec3{1.0};
    if (cells.x * cells.y * cells.z > 27.0) {
        return {.value = {.low = -farthest, .high = farthest}};
    }
    constexpr double infinity = std::numeric_limits<double>::infinity();
    NoiseBounds all{.value = {.low = infinity, .high = -infinity}, .sloped = true};
    all.slope.fill(all.value);
    const auto across = [](double count) { return static_cast<int>(count); };
    for (int z = 0; z < across(cells.z); ++z) {
        for (int y = 0; y < across(cells.y); ++y) {
            for (int x = 0; x < across(cells.x); ++x) {
                const DVec3 cell = first + DVec3{x, y, z};
                const NoiseBounds one = cell_bounds(cell, glm::max(low - cell, DVec3{0.0}),
                                                    glm::min(high - cell, DVec3{1.0}), seed);
                all.value = hull(all.value, one.value);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    all.slope.at(axis) = hull(all.slope.at(axis), one.slope.at(axis));
                }
            }
        }
    }
    return all;
}

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

Noise fbm(DVec3 point, const Octaves& octaves, double lod, std::uint64_t seed, Band band) noexcept {
    // Octaves whose wavelength is at least twice the detail asked for, and the next faded.
    const double shortest = std::max({2.0 * lod, octaves.finest, 1e-9});
    const double count = (maths::log(octaves.wavelength / shortest) / std::numbers::ln2) + 1.0;
    // A fixed rotation between octaves (orthonormal, no axis left in place).
    const glm::dmat3 turn{0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64};
    Noise sum;
    glm::dmat3 frame{1.0};
    double wavelength = octaves.wavelength;
    double amplitude = 1.0;
    for (int octave = 0;
         octave < 64 && static_cast<double>(octave) < count && wavelength > band.shorter_than;
         ++octave) {
        if (wavelength <= band.longest) {
            const double weight = std::min(1.0, count - static_cast<double>(octave));
            const Noise layer =
                gradient_noise(frame * point / wavelength, hash_values(seed, octave));
            sum.value += amplitude * weight * layer.value;
            // Back from the turned, scaled lattice to the point's own frame.
            sum.gradient +=
                (amplitude * weight / wavelength) * (glm::transpose(frame) * layer.gradient);
        }
        frame = turn * frame;
        wavelength *= 0.5;
        amplitude *= octaves.gain;
    }
    return sum;
}

Interval fbm_bounds(DVec3 middle, double within, const Octaves& octaves, double lod,
                    std::uint64_t seed, Band band) noexcept {
    // The same octaves, weights and turns as fbm().
    const double shortest = std::max({2.0 * lod, octaves.finest, 1e-9});
    const double count = (maths::log(octaves.wavelength / shortest) / std::numbers::ln2) + 1.0;
    const glm::dmat3 turn{0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64};
    Interval sum{};
    glm::dmat3 frame{1.0};
    double wavelength = octaves.wavelength;
    double amplitude = 1.0;
    for (int octave = 0;
         octave < 64 && static_cast<double>(octave) < count && wavelength > band.shorter_than;
         ++octave) {
        if (wavelength <= band.longest) {
            const double weight = std::min(1.0, count - static_cast<double>(octave));
            // A ball stays a ball turned and scaled.
            const DVec3 at = frame * middle / wavelength;
            const double across = within / wavelength;
            const std::uint64_t layer = hash_values(seed, octave);
            const NoiseBounds bounds = noise_bounds(at - DVec3{across}, at + DVec3{across}, layer);
            Interval one = bounds.value;
            // No further from the middle's value than the steepest it gets over the ball.
            if (bounds.sloped) {
                double steepest_here = 0.0;
                for (const Interval& slope : bounds.slope) {
                    const double most = std::max(std::abs(slope.low), std::abs(slope.high));
                    steepest_here += most * most;
                }
                const double middle_value = gradient_noise(at, layer).value;
                const double reaches = std::sqrt(steepest_here) * across;
                one = meet(one, {.low = middle_value - reaches, .high = middle_value + reaches});
            }
            // Rounding aside.
            constexpr double slack = 1e-12;
            sum = sum + ((amplitude * weight) *
                         Interval{.low = one.low - slack, .high = one.high + slack});
        }
        frame = turn * frame;
        wavelength *= 0.5;
        amplitude *= octaves.gain;
    }
    return sum;
}

double fbm_steepest(const Octaves& octaves) noexcept {
    // Every octave that's ever summed, at full weight: more than at any lod.
    const double shortest = std::max(octaves.finest, 1e-9);
    const double count = (maths::log(octaves.wavelength / shortest) / std::numbers::ln2) + 1.0;
    double sum = 0.0;
    double wavelength = octaves.wavelength;
    double amplitude = 1.0;
    for (int octave = 0; octave < 64 && static_cast<double>(octave) < count; ++octave) {
        sum += amplitude * steepest / wavelength;
        wavelength *= 0.5;
        amplitude *= octaves.gain;
    }
    return sum;
}

} // namespace hog::globe
