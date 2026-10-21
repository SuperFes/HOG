#include <hog/waves/shoaling.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace hog::waves {

namespace {

// The shallow wavenumber times the depth, at κ: Hunt's Padé fit to the dispersion relation, within
// about 0.2% of it, and cheap for the shaders.
double depth_wavenumber(double kappa) noexcept {
    constexpr std::array hunt{0.6666666667, 0.3555555556, 0.1608465608,
                              0.0632098765, 0.0217540484, 0.0065407983};
    double sum = 0.0;
    for (std::size_t i = hunt.size(); i-- > 0;) {
        sum = (sum + hunt.at(i)) * kappa;
    }
    return std::sqrt((kappa * kappa) + (kappa / (1.0 + sum)));
}

// A lag polynomial's value at κ and its slope against κ: over the shoreface's, whose slope has no
// constant term, it's taken over t first, so it stays finite where t is 0.
struct Lag {
    double value = 0.0;
    double slope = 0.0;
};

Lag lag_polynomial(const std::array<double, 9>& p, double kappa) noexcept {
    if (kappa >= deep_enough) {
        return {};
    }
    const double t = std::sqrt(std::max(kappa, 0.0) / deep_enough);
    double value = 0.0;
    double slope = 0.0;
    for (std::size_t n = p.size(); n-- > 0;) {
        slope = (slope * t) + value;
        value = (value * t) + p.at(n);
    }
    // d/dκ = d/dt / (2·deep_enough·t); the slope over t, when there's no term in t.
    if (p.at(1) == 0.0) {
        double over = 0.0;
        for (std::size_t n = p.size(); n-- > 2;) {
            over = (over * t) + (static_cast<double>(n) * p.at(n));
        }
        return {.value = value, .slope = over / (2.0 * deep_enough)};
    }
    return {.value = value, .slope = slope / (2.0 * deep_enough * t)};
}

// The shallowest water taken: the floor's profile has no slope at the waterline itself.
constexpr double shallowest = 0.01;

// The lag and its slope against κ, at κ for a wave of deep-water wavenumber `deep`.
Lag lag_at(double deep, double kappa) noexcept {
    const LagTerms terms = lag_terms(deep);
    if (kappa >= terms.turn) {
        const Lag shelf = lag_polynomial(shelf_lag, kappa);
        return {.value = shelf_run * shelf.value, .slope = shelf_run * shelf.slope};
    }
    const Lag shoreface = lag_polynomial(shoreface_lag, kappa);
    return {.value = terms.offset + (terms.scale * shoreface.value),
            .slope = terms.scale * shoreface.slope};
}

} // namespace

double shallow_wavenumber(double deep, double depth) noexcept {
    const double kappa = deep * std::max(depth, shallowest);
    return kappa >= deep_enough ? deep : depth_wavenumber(kappa) / std::max(depth, shallowest);
}

double shoaling(double deep, double depth) noexcept {
    const double kappa = deep * std::max(depth, shallowest);
    if (kappa >= deep_enough) {
        return 1.0;
    }
    const double kh = depth_wavenumber(kappa);
    // The dispersion relation makes tanh(kh) = κ/kh, so 2kh/sinh(2kh) = kh(1 - t²)/t; and the
    // group's speed over the phase's follows.
    const double t = kappa / kh;
    const double group = 0.5 * (1.0 + (kh * (1.0 - (t * t)) / t));
    return std::sqrt(1.0 / (2.0 * group * t));
}

double orbit_stretch(double deep, double depth) noexcept {
    const double kappa = deep * std::max(depth, shallowest);
    // 1/tanh(kh), as the dispersion relation has it.
    return kappa >= deep_enough ? 1.0 : depth_wavenumber(kappa) / kappa;
}

double phase_lag(double deep, double depth) noexcept {
    return lag_at(deep, deep * std::max(depth, shallowest)).value;
}

double phase_lag_slope(double deep, double depth) noexcept {
    return depth > shallowest ? deep * lag_at(deep, deep * depth).slope : 0.0;
}

LagTerms lag_terms(double deep) noexcept {
    const double turn = deep * shelf_depth;
    const double scale = 1.5 / (std::pow(dean_scale, 1.5) * std::sqrt(deep));
    return {.turn = turn,
            .scale = scale,
            .offset = (shelf_run * lag_polynomial(shelf_lag, turn).value) -
                      (scale * lag_polynomial(shoreface_lag, turn).value)};
}

double depth_limited(double height, double depth) noexcept {
    const double most = breaking_depth * std::max(depth, 0.0);
    return height > most ? most / height : 1.0;
}

} // namespace hog::waves
