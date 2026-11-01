#include "terms.hpp"

#include <algorithm>
#include <cmath>

namespace hog::globe::terms {
namespace {

// Earth's mountain belts lie some 3,000 km apart; a belt must still be wide enough for its peaks
// to stand at a slope rock holds, so on a small world they're never closer than ten times the
// tallest peak.
constexpr double belt_spacing = 3.0e6;
constexpr double belt_spacing_per_height = 10.0;
// Of the belt field's ±1, how near zero counts as in a belt.
constexpr double belt_width = 0.22;

// Peaks within a belt are about this many times as far apart as the tallest is high.
constexpr double peak_spacing_per_height = 2.5;
// Fractal octave sums reach about half again their first octave's amplitude.
constexpr double octave_reach = 1.5;

// The rolling ground everywhere: Earth's plains rise and fall tens of metres over kilometres.
constexpr double hill_wavelength = 2'000.0;
constexpr double hill_height = 40.0;
// Ground rougher than self-similar slopes: real terrain's Hurst exponent is about 0.8.
constexpr double rough_gain = 0.57;

// Mid-ocean ridges stand about half the isostatic step over the abyssal plains.
constexpr double ridge_share = 0.45;
constexpr double ridge_width = 0.35;

// The widest gullies are a quarter of the peaks' spacing. Each is cut no deeper than a share of
// how far the ground falls across its width, walls a little steeper than the slope they're cut
// in; each finer set takes a smaller share, so their slopes add up to a steep wall, not a jagged
// one. None narrower than four voxels: below that the ground's grain is the soil's.
constexpr double gully_first_per_spacing = 0.25;
constexpr double share_of_fall = 0.3;
constexpr double share_falloff = 0.75;
constexpr double narrowest_gully_voxels = 4.0;

// How much mountain building each regime does: plates build belts on the continents, an
// episodic lid wrinkles everything moderately, a stagnant lid only keeps old worn swells.
struct Building {
    double on_high = 0.0;
    double on_low = 0.0;
};

Building building(Regime regime) {
    switch (regime) {
    case Regime::Plates:
        return {.on_high = 1.0, .on_low = 0.0};
    case Regime::Episodic:
        return {.on_high = 0.5, .on_low = 0.5};
    case Regime::StagnantLid:
        return {.on_high = 0.25, .on_low = 0.25};
    }
    return {};
}

// 1 on the field's zero line, falling smoothly to 0 at ±width: long sinuous lines.
Noise line(const Noise& field, double width) {
    const double x = 1.0 - ((field.value * field.value) / (width * width));
    return {.value = smoothstep(x),
            .gradient =
                (smoothstep_slope(x) * -2.0 * field.value / (width * width)) * field.gradient};
}

Noise scaled(const Noise& noise, double by) {
    return {.value = noise.value * by, .gradient = noise.gradient * by};
}

Noise product(const Noise& a, const Noise& b) {
    return {.value = a.value * b.value,
            .gradient = (a.gradient * b.value) + (b.gradient * a.value)};
}

void add(Noise& sum, const Noise& more) {
    sum.value += more.value;
    sum.gradient += more.gradient;
}

} // namespace

Noise relief(const Context& context, DVec3 direction, const Province& crust, double lod) noexcept {
    const Planet& planet = *context.planet;
    const Derived& derived = *context.derived;
    const DVec3 at = direction * planet.radius;
    const double tallest = derived.tallest;
    Noise height;

    // Where mountains are built, and how much.
    const Building build = building(planet.regime);
    const double spacing =
        std::max(belt_spacing * derived.lateral, belt_spacing_per_height * tallest);
    const Noise belt_field = fbm(at, {.wavelength = spacing, .gain = 0.5, .finest = spacing / 4.0},
                                 0.0, context.seed(Term::Belts));
    const Noise building_here{.value = build.on_low + ((build.on_high - build.on_low) * crust.high),
                              .gradient = (build.on_high - build.on_low) * crust.gradient};
    // Half the mountains' reach stands as the belt's bulk, the peaks' octaves rise and fall on it.
    const Noise mountains =
        scaled(product(line(belt_field, belt_width), building_here), 0.5 * tallest / octave_reach);
    add(height, mountains);
    const Noise hill_scale{.value = hill_height * (1.0 + crust.high),
                           .gradient = hill_height * crust.gradient};

    // Peaks and hills a band of octaves at a time, so gullies can be cut between them.
    const Octaves peaks{.wavelength = peak_spacing_per_height * tallest,
                        .gain = rough_gain,
                        .finest = planet.voxel};
    const Octaves hills{.wavelength = hill_wavelength, .gain = rough_gain, .finest = planet.voxel};
    const auto band = [&](Band range) {
        Noise sum;
        if (mountains.value > 0.0) {
            add(sum, product(fbm(at, peaks, lod, context.seed(Term::Peaks), range), mountains));
        }
        add(sum, product(fbm(at, hills, lod, context.seed(Term::Hills), range), hill_scale));
        return sum;
    };

    if (!derived.fluvial) {
        add(height, band({}));
    } else {
        // Coarse to fine: each width of gullies runs down the ground coarser than itself, so the
        // finer ones feed the coarser valleys as tributaries do.
        double wavelength = gully_first_per_spacing * peaks.wavelength;
        add(height, band({.shorter_than = wavelength}));
        const double narrowest = std::max(2.0 * lod, narrowest_gully_voxels * planet.voxel);
        double share = share_of_fall;
        for (int octave = 0; octave < 32 && wavelength >= narrowest; ++octave) {
            const double weight = std::min(1.0, (wavelength / narrowest) - 1.0);
            add(height, gullies(context, direction, tangential(height.gradient, direction),
                                wavelength, octave, share, weight));
            add(height, band({.longest = wavelength, .shorter_than = 0.5 * wavelength}));
            wavelength *= 0.5;
            share *= share_falloff;
        }
        add(height, band({.longest = wavelength}));
    }

    // Ridges down the middle of the ocean floors, where plates spread.
    if (planet.regime == Regime::Plates) {
        const double wavelength = 6.0e6 * derived.lateral;
        const Noise ridge_field =
            fbm(at, {.wavelength = wavelength, .gain = 0.5, .finest = wavelength / 8.0}, 0.0,
                context.seed(Term::Ridges));
        const Noise low{.value = 1.0 - crust.high, .gradient = -crust.gradient};
        add(height, scaled(product(line(ridge_field, ridge_width), low),
                           ridge_share * derived.isostatic_step));
    }

    height.gradient = tangential(height.gradient, direction);
    return height;
}

} // namespace hog::globe::terms
