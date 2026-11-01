#include "terms.hpp"

#include <hog/core/hash.hpp>

#include <algorithm>
#include <cmath>

namespace hog::globe::terms {
namespace {

// Earth's continents run some 6,000 km across; the margins between them are as wide as their
// slope lets the isostatic step stand, about one in ten for shelf sediment, and no narrower than
// Earth's 200 km of shelf, slope and rise scaled to the planet.
constexpr double continent_wavelength = 6.0e6;
constexpr double margin_slope = 0.1;
constexpr double margin_width = 2.0e5;

// How hard the province field leans toward one side when the high crust gathers there, against
// its own spread of about ±0.3.
constexpr double one_side_lean = 1.2;

} // namespace

std::uint64_t Context::seed(Term term) const noexcept {
    return hash_values(planet->seed, static_cast<std::uint64_t>(term));
}

DVec3 tangential(DVec3 gradient, DVec3 direction) noexcept {
    return gradient - (glm::dot(gradient, direction) * direction);
}

double smoothstep(double x) noexcept {
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

double smoothstep_slope(double x) noexcept {
    const double t = std::clamp(x, 0.0, 1.0);
    return 6.0 * t * (1.0 - t);
}

Noise province_field(const Context& context, DVec3 direction) noexcept {
    const double radius = context.planet->radius;
    const double wavelength = continent_wavelength * context.derived->lateral;
    const Octaves octaves{.wavelength = wavelength, .gain = 0.5, .finest = margin(context) / 4.0};
    Noise field = fbm(direction * radius, octaves, 0.0, context.seed(Term::Province));
    if (context.planet->layout == Layout::OneSide) {
        field.value += one_side_lean * glm::dot(direction, context.shape->one_side);
        field.gradient += (one_side_lean / radius) * context.shape->one_side;
    }
    field.gradient = tangential(field.gradient, direction);
    return field;
}

Province province(const Context& context, DVec3 direction) noexcept {
    const Noise field = province_field(context, direction);
    // A fixed band of the field, so margins run as wide as the step's slope where the field
    // crosses at its usual steepness, and narrower where it crosses steeply.
    const double band = context.shape->province_band;
    const double across = ((field.value - context.shape->province_threshold) / band) + 0.5;
    return {.high = smoothstep(across),
            .gradient = (smoothstep_slope(across) / band) * field.gradient};
}

double margin(const Context& context) noexcept {
    return std::max(margin_width * context.derived->lateral,
                    std::abs(context.derived->isostatic_step) / margin_slope);
}

} // namespace hog::globe::terms
