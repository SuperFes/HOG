#include "dissection.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/maths.hpp>
#include <hog/globe/noise.hpp>

#include <algorithm>
#include <array>
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
// Gullies wind as the ground they cut through varies, the ground about them pushed aside by up
// to this share of the widest gullies' spacing, so finer ones bend along with the wider ones,
// bending over a few spacings and never so sharply that neighbours cross.
constexpr double winding = 0.35;
constexpr double bends_over = 4.0;
constexpr double sharpest_bend = 0.5;

// The stripes the gullies about make, running across `across`: how far along them, as an angle,
// from a crest at 0 to a floor at ±π, and how strongly they agree.
struct Stripes {
    double phase = 0.0;
    double strength = 0.0;
    // How both change, per cell: the phase turns once a cell where one gully rules, faster where
    // gullies out of step meet and branch.
    DVec3 phase_gradient{0.0};
    DVec3 strength_gradient{0.0};
};

Stripes stripes(DVec3 cells, DVec3 across, std::uint64_t seed) noexcept {
    const DVec3 middle = glm::floor(cells + 0.5);
    // Cells whose points can't lie within reach, scattered however they are, are never hashed.
    constexpr double farthest = reach + (jitter * std::numbers::sqrt3);
    double real = 0.0;
    double imaginary = 0.0;
    // How the weights change, turned with their stripes.
    DVec3 weighted_real{0.0};
    DVec3 weighted_imaginary{0.0};
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
                const DVec3 weight_gradient = off * (-4.0 * (1.0 - near) / (reach * reach));
                const double turn = 2.0 * std::numbers::pi * glm::dot(off, across);
                const double cosine = maths::cos(turn);
                const double sine = maths::sin(turn);
                real += weight * cosine;
                imaginary += weight * sine;
                weighted_real += weight_gradient * cosine;
                weighted_imaginary += weight_gradient * sine;
            }
        }
    }
    Stripes made{.phase = maths::atan2(imaginary, real), .strength = std::hypot(real, imaginary)};
    if (made.strength > 0.0) {
        const double power = made.strength * made.strength;
        made.phase_gradient = (across * (2.0 * std::numbers::pi)) +
                              (((weighted_imaginary * real) - (weighted_real * imaginary)) / power);
        made.strength_gradient =
            ((weighted_real * real) + (weighted_imaginary * imaginary)) / made.strength;
    }
    return made;
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

// A smooth step from 0 to 1 as x rises to `edge`, and how steeply.
std::pair<double, double> smoothstep(double edge, double x) noexcept {
    const double t = std::clamp(x / edge, 0.0, 1.0);
    return {t * t * (3.0 - (2.0 * t)), 6.0 * t * (1.0 - t) / edge};
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
    // How both change over the ground, so a finer gully's depth following them is told.
    DVec3 room_gradient = ground.above_floor_gradient;
    DVec3 under_crest_gradient{0.0};
    double cut = 0.0;
    // Which way the ground about turns them, in their spacings.
    const DVec3 metres = direction * radius;
    const Octaves turning{
        .wavelength = bends_over * gullies.longest, .gain = 0.5, .finest = gullies.longest / 2.0};
    const double pushed = std::min(winding * gullies.longest,
                                   sharpest_bend / (std::numbers::sqrt3 * fbm_steepest(turning)));
    const std::array<Noise, 3> turns{fbm(metres, turning, lod, hash_values(gullies.seed, 0x77)),
                                     fbm(metres, turning, lod, hash_values(gullies.seed, 0x78)),
                                     fbm(metres, turning, lod, hash_values(gullies.seed, 0x79))};
    const DVec3 wound = DVec3{turns[0].value, turns[1].value, turns[2].value} * pushed;
    // A way through the pushed ground back to the ground's own.
    const auto unwound = [&](DVec3 way) {
        const DVec3 back = way + ((turns[0].gradient * way.x) + (turns[1].gradient * way.y) +
                                  (turns[2].gradient * way.z)) *
                                     pushed;
        return back - (direction * glm::dot(back, direction));
    };
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
        const double held = 2.0 * std::min(room, under_crest);
        double depth = std::min(slope * wavelength, held) * fade;
        if (depth <= 0.0 || across_room <= 0.0) {
            continue;
        }
        DVec3 depth_gradient =
            held < slope * wavelength
                ? (room < under_crest ? room_gradient : under_crest_gradient) * (2.0 * fade)
                : DVec3{0.0};
        const DVec3 across = glm::cross(direction, fall) / slope;
        const Stripes made =
            stripes((metres + wound) / wavelength, across, hash_values(gullies.seed, octave));
        // Per metre over the ground; the walls are as steep as the phase turns, however close
        // the stripes run, and no steeper than what's left across.
        const DVec3 phase_gradient = unwound(made.phase_gradient) / wavelength;
        const double turning_rate = glm::length(phase_gradient);
        const double walls = across_room * std::numbers::pi * (1.0 - rounding) * fade;
        if (depth * turning_rate > walls) {
            depth = walls / turning_rate;
            depth_gradient = DVec3{0.0};
        }
        const auto [kept, keeping] = smoothstep(in_step, made.strength);
        const auto [down, steepness] = profile(std::abs(made.phase) / std::numbers::pi);
        const double offset = depth * kept * (0.5 - down);
        height += offset;
        const DVec3 wall =
            phase_gradient * ((made.phase < 0.0 ? 1.0 : -1.0) * depth * kept / std::numbers::pi);
        depth_gradient = (depth_gradient * kept) +
                         (unwound(made.strength_gradient) * (depth * keeping / wavelength));
        depth *= kept;
        const DVec3 offset_gradient = (wall * steepness) + (depth_gradient * (0.5 - down));
        const DVec3 down_gradient =
            phase_gradient * ((made.phase < 0.0 ? -1.0 : 1.0) * steepness / std::numbers::pi);
        gradient += offset_gradient;
        fall += wall / (1.0 - rounding);
        if (room + offset < depth * (1.0 - down)) {
            room += offset;
            room_gradient += offset_gradient;
        } else {
            room = depth * (1.0 - down);
            room_gradient = (depth_gradient * (1.0 - down)) - (down_gradient * depth);
        }
        under_crest = depth * down;
        under_crest_gradient = (depth_gradient * down) + (down_gradient * depth);
        cut += depth / 2.0;
    }

    Dissected dissected{.top = {.value = height + ground.ice, .gradient = gradient},
                        .ice = ground.ice,
                        .cut = bed - height};
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
