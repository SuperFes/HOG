#include <hog/waves/foam.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace hog::waves {

namespace {

// A crest breaks white for about a second, and the foam it leaves thins away over a few more; so
// much of what's white is each.
constexpr double breaking_lasts = 1.0; // s
constexpr double left_lasts = 3.0;

// The wind's waves run no faster than it, give or take; those much slower than it are too short
// to break white.
constexpr double outrun = 1.6;  // their speed over the wind's, from which they take no part
constexpr double kept_up = 1.2; // from which they take full part
constexpr double short_from = 0.33;
constexpr double too_short = 0.2;

// The wind drags the water at its surface along at about this much of its own speed.
constexpr double dragged = 0.03;

double smoothstep(double from, double to, double at) noexcept {
    const double t = std::clamp((at - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

// The mean square slope of the waves that break, of all the sea's systems.
double breaking_slope(const SeaState& sea, double speed) {
    double sum = 0.0;
    for (const System& system : sea.systems) {
        const std::array<double, bands> variances = band_variances(system);
        for (int band = 0; band < bands; ++band) {
            const double frequency = band_lowest(band) + (band_width(band) / 2.0);
            const double wavenumber = frequency * frequency / gravity;
            const double share = breaking_share(frequency, speed);
            sum += share * share * wavenumber * wavenumber *
                   variances.at(static_cast<std::size_t>(band));
        }
    }
    return sum;
}

} // namespace

Whitecaps whitecaps(double speed) noexcept {
    if (!(speed > 0.0)) {
        return {};
    }
    const double white = std::min(3.84e-6 * std::pow(speed, 3.41), 1.0);
    const double breaking = white * breaking_lasts / (breaking_lasts + left_lasts);
    return {.breaking = breaking, .left = white - breaking};
}

double breaking_share(double frequency, double speed) noexcept {
    if (!(speed > 0.0) || !(frequency > 0.0)) {
        return 0.0;
    }
    // A wave's speed over the wind's.
    const double age = gravity / (frequency * speed);
    return smoothstep(outrun, kept_up, age) * smoothstep(too_short, short_from, age);
}

Whitecaps whitecaps(const SeaState& sea) {
    const double speed = glm::length(sea.wind);
    const Whitecaps open = whitecaps(speed);
    if (open.breaking <= 0.0) {
        return {};
    }
    const double full =
        breaking_slope({.systems = {grown_sea(sea.wind)}, .current = {}, .wind = {}}, speed);
    if (!(full > 0.0)) {
        return {};
    }
    const double grown = std::min(breaking_slope(sea, speed) / full, 1.0);
    return {.breaking = open.breaking * grown, .left = open.left * grown};
}

double exceeded(double share) noexcept {
    const double within = std::clamp(share, 1e-9, 1.0 - 1e-9);
    // How often a standard normal value is over x falls as x rises, so bisect for it.
    double low = -7.0;
    double high = 7.0;
    for (int step = 0; step < 60; ++step) {
        const double middle = (low + high) / 2.0;
        if (std::erfc(middle / std::numbers::sqrt2) / 2.0 > within) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return (low + high) / 2.0;
}

DVec3 foam_drift(const SeaState& sea) noexcept {
    return sea.current + (sea.wind * dragged);
}

double streaking(double speed) noexcept {
    // From Beaufort's force 7, when the foam begins to be blown in streaks, to 8, when they're
    // well marked.
    constexpr double most = 8.0;
    return 1.0 + ((most - 1.0) * smoothstep(14.0, 19.0, speed));
}

} // namespace hog::waves
