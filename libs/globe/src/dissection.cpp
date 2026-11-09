#include "dissection.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/maths.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace hog::globe {

namespace {

// Each gully's line passes through a point scattered about a cell of a lattice its spacing
// across, counting out to `reach` cells by a bump falling smoothly to nothing there.
constexpr double reach = 1.25;
constexpr double jitter = 0.25;
// Crests and floors are rounded over this share of the way from one to the other.
constexpr double rounding = 0.25;
// Where the gullies about cross out of step, they fade out, branching there.
constexpr double in_step = 1.0;

// The stripes the gullies about make, running across `across`: how far along them, as an angle,
// from a crest at 0 to a floor at ±π, and how strongly they agree.
struct Stripes {
    double phase = 0.0;
    double strength = 0.0;
};

Stripes stripes(DVec3 cells, DVec3 across, std::uint64_t seed) noexcept {
    const DVec3 middle = glm::floor(cells + 0.5);
    // Cells whose points can't lie within reach, scattered however they are, are never hashed.
    constexpr double farthest = reach + (jitter * std::numbers::sqrt3);
    double real = 0.0;
    double imaginary = 0.0;
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                const DVec3 cell = middle + DVec3{dx, dy, dz};
                const DVec3 from_cell = cells - cell;
                if (glm::dot(from_cell, from_cell) >= farthest * farthest) {
                    continue;
                }
                const std::uint64_t bits = hash_values(seed, static_cast<std::int64_t>(cell.x),
                                                       static_cast<std::int64_t>(cell.y),
                                                       static_cast<std::int64_t>(cell.z));
                const auto scatter = [&](unsigned shift) {
                    return ((static_cast<double>((bits >> shift) & 0x1FFFFFU) * 0x1.0p-20) - 1.0) *
                           jitter;
                };
                const DVec3 off = from_cell - DVec3{scatter(0), scatter(21), scatter(42)};
                const double near = glm::dot(off, off) / (reach * reach);
                if (near >= 1.0) {
                    continue;
                }
                const double weight = (1.0 - near) * (1.0 - near);
                const double turn = 2.0 * std::numbers::pi * glm::dot(off, across);
                real += weight * maths::cos(turn);
                imaginary += weight * maths::sin(turn);
            }
        }
    }
    return {.phase = maths::atan2(imaginary, real), .strength = std::hypot(real, imaginary)};
}

// How far down from a crest at 0 to a floor at 1, both rounded, and how steeply, per unit.
std::pair<double, double> profile(double along) noexcept {
    constexpr double scale = 1.0 / (1.0 - rounding);
    if (along < rounding) {
        return {along * along / (2.0 * rounding) * scale, along / rounding * scale};
    }
    if (along > 1.0 - rounding) {
        const double left = 1.0 - along;
        return {(1.0 - rounding - (left * left / (2.0 * rounding))) * scale,
                left / rounding * scale};
    }
    return {(along - (rounding / 2.0)) * scale, scale};
}

double smoothstep(double edge, double x) noexcept {
    const double t = std::clamp(x / edge, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

} // namespace

Dissected dissect(DVec3 direction, double radius, const Undissected& ground, const Gullies& gullies,
                  double lod) noexcept {
    const double bed = ground.top.value - ground.ice;
    double height = bed;
    DVec3 gradient = ground.top.gradient;
    DVec3 fall = ground.fall;
    // How far over the floors below and under the crests above each finer gully may cut: they
    // run down the straight walls between, which turn where they meet.
    double room = ground.above_floor;
    double under_crest = 1e30;
    double cut = 0.0;
    for (int octave = 0; octave < gullies.octaves; ++octave) {
        const double wavelength = std::ldexp(gullies.longest, -octave);
        if (wavelength < 2.0 * lod) {
            break;
        }
        const double fade = std::min(1.0, (wavelength / (2.0 * lod)) - 1.0);
        const double slope = glm::length(fall);
        // What's left of the steepest across the way down, so the two together are no steeper;
        // taken from the length rather than its square, which would swing wildly near none left.
        const double across_room = std::max(0.0, ground.steepest - slope);
        double depth = std::min({across_room * wavelength / 2.0 * (1.0 - rounding),
                                 slope * wavelength, 2.0 * std::min(room, under_crest)}) *
                       fade;
        if (depth <= 0.0) {
            continue;
        }
        const DVec3 across = glm::cross(direction, fall) / slope;
        const Stripes made =
            stripes(direction * (radius / wavelength), across, hash_values(gullies.seed, octave));
        depth *= smoothstep(in_step, made.strength);
        const auto [down, steepness] = profile(std::abs(made.phase) / std::numbers::pi);
        const double offset = depth * (0.5 - down);
        const double side = (made.phase < 0.0 ? 1.0 : -1.0) * depth * 2.0 / wavelength;
        height += offset;
        gradient += across * (side * steepness);
        fall += across * (side / (1.0 - rounding));
        room = std::min(room + offset, depth * (1.0 - down));
        under_crest = depth * down;
        cut += depth / 2.0;
    }

    Dissected dissected{.top = {.value = height + ground.ice, .gradient = gradient},
                        .ice = ground.ice};
    if (ground.ice > 0.0) {
        // Ice as deep as it lay over the uncut bed settles into what's cut, its top level: over
        // all of it if as deep as the cut, else to where it fills the gullies, crests out of it.
        const double settled =
            ground.ice >= cut ? ground.ice : (2.0 * std::sqrt(cut * ground.ice)) - cut;
        const double surface = bed + settled;
        dissected.top = height < surface ? Noise{.value = surface, .gradient = ground.top.gradient}
                                         : Noise{.value = height, .gradient = gradient};
        dissected.ice = std::max(0.0, surface - height);
    }
    return dissected;
}

} // namespace hog::globe
