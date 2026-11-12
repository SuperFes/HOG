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

// The stripes the gullies about make, running across `across` over ground facing `up`: how far
// along them, as an angle, from a crest at 0 to a floor at ±π, and how strongly they agree. Each
// gully's stripes run a little apart from the rest, turned a little their own way and spaced
// a little wider or closer, so none lie evenly side by side.
struct Stripes {
    double phase = 0.0;
    double strength = 0.0;
    // How both change, per cell: the phase turns once a cell where one gully rules, faster where
    // gullies out of step meet and branch.
    DVec3 phase_gradient{0.0};
    DVec3 strength_gradient{0.0};
    // How the phase's gradient changes, per cell.
    glm::dmat3 phase_hessian{0.0};
};

Stripes stripes(DVec3 cells, DVec3 across, DVec3 up, std::uint64_t seed) noexcept {
    const DVec3 middle = glm::floor(cells + 0.5);
    // Cells whose points can't lie within reach, scattered however they are, are never hashed.
    constexpr double farthest = reach + (jitter * std::numbers::sqrt3);
    constexpr double tau = 2.0 * std::numbers::pi;
    // How far a gully's stripes may turn from the rest, in radians, and space themselves apart,
    // as a power of two either way.
    constexpr double turned = 0.25;
    constexpr double spaced = 0.4;
    const DVec3 aside = glm::cross(up, across);
    double real = 0.0;
    double imaginary = 0.0;
    DVec3 real_gradient{0.0};
    DVec3 imaginary_gradient{0.0};
    glm::dmat3 real_curve{0.0};
    glm::dmat3 imaginary_curve{0.0};
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
                const std::uint64_t own = hash_values(bits, 0x6f776eULL);
                const auto spread = [&](unsigned shift) {
                    return (static_cast<double>((own >> shift) & 0xFFFFU) * 0x1.0p-15) - 1.0;
                };
                const double turning = spread(0) * turned;
                const DVec3 way = ((across * maths::cos(turning)) + (aside * maths::sin(turning))) *
                                  std::exp2(spread(16) * spaced);
                const double weight = (1.0 - near) * (1.0 - near);
                const DVec3 weight_gradient = off * (-4.0 * (1.0 - near) / (reach * reach));
                const glm::dmat3 weight_curve =
                    (glm::dmat3{1.0} * (-4.0 * (1.0 - near) / (reach * reach))) +
                    (glm::outerProduct(off, off) * (8.0 / (reach * reach * reach * reach)));
                const double turn = tau * glm::dot(off, way);
                const DVec3 turn_gradient = way * tau;
                const double cosine = maths::cos(turn);
                const double sine = maths::sin(turn);
                const glm::dmat3 crossed = glm::outerProduct(weight_gradient, turn_gradient) +
                                           glm::outerProduct(turn_gradient, weight_gradient);
                const glm::dmat3 turned_twice =
                    glm::outerProduct(turn_gradient, turn_gradient) * weight;
                real += weight * cosine;
                imaginary += weight * sine;
                real_gradient += (weight_gradient * cosine) - (turn_gradient * (weight * sine));
                imaginary_gradient +=
                    (weight_gradient * sine) + (turn_gradient * (weight * cosine));
                real_curve += (weight_curve * cosine) - (crossed * sine) - (turned_twice * cosine);
                imaginary_curve +=
                    (weight_curve * sine) + (crossed * cosine) - (turned_twice * sine);
            }
        }
    }
    Stripes made{.phase = maths::atan2(imaginary, real), .strength = std::hypot(real, imaginary)};
    if (made.strength > 0.0) {
        const double power = made.strength * made.strength;
        const DVec3 share = (imaginary_gradient * real) - (real_gradient * imaginary);
        made.phase_gradient = share / power;
        made.strength_gradient =
            ((real_gradient * real) + (imaginary_gradient * imaginary)) / made.strength;
        const glm::dmat3 share_gradient = glm::outerProduct(imaginary_gradient, real_gradient) -
                                          glm::outerProduct(real_gradient, imaginary_gradient) +
                                          (imaginary_curve * real) - (real_curve * imaginary);
        const DVec3 power_gradient =
            ((real_gradient * real) + (imaginary_gradient * imaginary)) * 2.0;
        made.phase_hessian =
            (share_gradient / power) - (glm::outerProduct(share, power_gradient) / (power * power));
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

// How much of the depth it may cut the gully of octave `octave`, `wavelength` apart, takes about
// `metres`: some cut deeper than others about them. Its gradient per metre along the ground.
Noise depth_share(DVec3 metres, DVec3 direction, double wavelength, const Gullies& gullies,
                  int octave, double lod) noexcept {
    const Noise deeper =
        fbm(metres, {.wavelength = 2.0 * wavelength, .gain = 0.5, .finest = wavelength}, lod,
            hash_values(gullies.seed, 0x64, octave));
    const double share = 0.55 + (0.45 * deeper.value);
    if (share <= 0.0) {
        return {};
    }
    return {.value = share,
            .gradient =
                (deeper.gradient - (direction * glm::dot(deeper.gradient, direction))) * 0.45};
}

// How far either side of the uncut bed the gullies about cut it on the whole: half as deep as the
// widest of them cut, as the ground falls over its width, its walls let it stand and its floor
// lets it reach, however it winds and wherever along it, as deep as those about here cut; the
// finer ones, held by its floors and crests, cut within that. Left untold: how that changes with
// the ground's own fall.
Noise spread_about(DVec3 metres, DVec3 direction, const Undissected& ground, const Gullies& gullies,
                   double lod) noexcept {
    const double slope = glm::length(ground.fall);
    // Walls as steep as what's left across, the phase turning once a gully's width.
    const double walls = std::max(0.0, ground.steepest - slope) * (1.0 - rounding) / 2.0;
    if (gullies.octaves <= 0 || gullies.longest < 2.0 * lod) {
        return {};
    }
    const double fade = std::min(1.0, (gullies.longest / (2.0 * lod)) - 1.0);
    const double falls = std::min(slope, walls) * gullies.longest;
    const Noise share = depth_share(metres, direction, gullies.longest, gullies, 0, lod);
    if (2.0 * ground.above_floor < falls) {
        return {.value = ground.above_floor * fade * share.value,
                .gradient = ((ground.above_floor_gradient * share.value) +
                             (share.gradient * ground.above_floor)) *
                            fade};
    }
    return {.value = falls * fade * share.value / 2.0,
            .gradient = share.gradient * (falls * fade / 2.0)};
}

} // namespace

Settled settled(double thick, double spread) noexcept {
    if (thick >= spread) {
        return {.level = thick, .per_thick = 1.0, .per_spread = 0.0};
    }
    if (thick <= 0.0) {
        return {.level = -spread, .per_thick = 0.0, .per_spread = -1.0};
    }
    // Over the lowest of the evenly spread bed it lies as a wedge: (level + spread)² / 4 spread.
    const double share = std::sqrt(thick / spread);
    return {.level = (2.0 * std::sqrt(thick * spread)) - spread,
            .per_thick = 1.0 / share,
            .per_spread = share - 1.0};
}

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
    // Which way the ground about turns them, in their spacings, down to the finest's.
    const DVec3 metres = direction * radius;
    const Octaves turning{.wavelength = bends_over * gullies.longest,
                          .gain = 0.45,
                          .finest = std::ldexp(gullies.longest, -gullies.octaves)};
    const double pushed = std::min(winding * gullies.longest,
                                   sharpest_bend / (std::numbers::sqrt3 * fbm_steepest(turning)));
    const std::array<Noise, 3> turns{fbm(metres, turning, lod, hash_values(gullies.seed, 0x77)),
                                     fbm(metres, turning, lod, hash_values(gullies.seed, 0x78)),
                                     fbm(metres, turning, lod, hash_values(gullies.seed, 0x79))};
    const DVec3 wound = DVec3{turns[0].value, turns[1].value, turns[2].value} * pushed;
    // A way through the pushed ground back to the ground's own, over the ground.
    glm::dmat3 wound_by{1.0};
    for (glm::length_t axis = 0; axis < 3; ++axis) {
        wound_by[axis] += turns[axis].gradient * pushed;
    }
    const glm::dmat3 unwinding =
        (glm::dmat3{1.0} - glm::outerProduct(direction, direction)) * wound_by;
    const auto unwound = [&](DVec3 way) { return unwinding * way; };
    // How the way down changes, per metre, as the gullies cut so far turn it; the ground's own
    // turning is left untold.
    glm::dmat3 bending{0.0};
    for (int octave = 0; octave < gullies.octaves; ++octave) {
        const double wavelength = std::ldexp(gullies.longest, -octave);
        if (wavelength < 2.0 * lod) {
            break;
        }
        const double fade = std::min(1.0, (wavelength / (2.0 * lod)) - 1.0);
        const double slope = glm::length(fall);
        const DVec3 steepening =
            slope > 0.0 ? glm::transpose(bending) * (fall / slope) : DVec3{0.0};
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
                : steepening * (wavelength * fade);
        // Each runs the way the ground falls smoothed over its own width, the widest as it falls
        // on the whole, the finest as it falls here: turned faster than that, the stripes of
        // gullies a cell off would swing about faster than they run.
        const double broad =
            std::clamp(std::log2(wavelength * 8.0 / gullies.longest) / 2.0, 0.0, 1.0);
        const DVec3 led = (ground.fall * (1.0 - broad)) + (ground.broad_fall * broad);
        const double leading = glm::length(led);
        const DVec3 across = leading > 0.0 ? glm::cross(direction, led) / leading
                                           : glm::cross(direction, fall) / slope;
        const Stripes made = stripes((metres + wound) / wavelength, across, direction,
                                     hash_values(gullies.seed, octave));
        // Per metre over the ground; the walls are as steep as the phase turns, however close
        // the stripes run, and no steeper than what's left across.
        const DVec3 phase_gradient = unwound(made.phase_gradient) / wavelength;
        const glm::dmat3 phase_curve =
            unwinding * made.phase_hessian * glm::transpose(unwinding) / (wavelength * wavelength);
        const double turning_rate = glm::length(phase_gradient);
        const double walls = across_room * std::numbers::pi * (1.0 - rounding) * fade;
        if (depth * turning_rate > walls) {
            // As deep as the stripes run apart, deepening as they part.
            depth = walls / turning_rate;
            const DVec3 walls_gradient = steepening * (-std::numbers::pi * (1.0 - rounding) * fade);
            const DVec3 turning_gradient = phase_curve * phase_gradient / turning_rate;
            depth_gradient = (walls_gradient - (turning_gradient * depth)) / turning_rate;
        }
        const Noise share = depth_share(metres, direction, wavelength, gullies, octave, lod);
        depth_gradient = (depth_gradient * share.value) + (share.gradient * depth);
        depth *= share.value;
        const auto [kept, keeping] = smoothstep(in_step, made.strength);
        const auto [down, steepness] = profile(std::abs(made.phase) / std::numbers::pi);
        const double offset = depth * kept * (0.5 - down);
        height += offset;
        const double side = made.phase < 0.0 ? 1.0 : -1.0;
        const DVec3 wall = phase_gradient * (side * depth * kept / std::numbers::pi);
        depth_gradient = (depth_gradient * kept) +
                         (unwound(made.strength_gradient) * (depth * keeping / wavelength));
        depth *= kept;
        bending += ((phase_curve * depth) + glm::outerProduct(phase_gradient, depth_gradient)) *
                   (side / (std::numbers::pi * (1.0 - rounding)));
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
                        .cut = bed - height,
                        .cut_gradient = ground.top.gradient - gradient,
                        .spread = spread_about(metres, direction, ground, gullies, lod)};
    if (ground.ice > 0.0) {
        // Ice as deep as it lay over the uncut bed settles into what's cut, its top level: over
        // all of it if as deep as the cut, else to where it fills the gullies, crests out of it.
        const double surface = bed + settled(ground.ice, cut).level;
        dissected.top = height < surface ? Noise{.value = surface, .gradient = ground.top.gradient}
                                         : Noise{.value = height, .gradient = gradient};
        dissected.ice = std::max(0.0, surface - height);
    }
    return dissected;
}

Interval gully_bounds(DVec3 metres, double within, double steepest, const Gullies& gullies,
                      double lod) noexcept {
    // Each gully cuts down and stands up half its depth at most; it's no deeper than the ground
    // falls over its width, which is less than its walls stand, times its share of that.
    double most = 0.0;
    for (int octave = 0; octave < gullies.octaves; ++octave) {
        const double wavelength = std::ldexp(gullies.longest, -octave);
        if (wavelength < 2.0 * lod) {
            break;
        }
        const double fade = std::min(1.0, (wavelength / (2.0 * lod)) - 1.0);
        const Interval deeper = fbm_bounds(
            metres, within, {.wavelength = 2.0 * wavelength, .gain = 0.5, .finest = wavelength},
            lod, hash_values(gullies.seed, 0x64, octave));
        const double share = std::max(0.0, 0.55 + (0.45 * deeper.high));
        most += 0.5 * steepest * wavelength * fade * share;
    }
    return {.low = -most, .high = most};
}

} // namespace hog::globe
