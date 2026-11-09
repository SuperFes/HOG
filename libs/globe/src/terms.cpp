#include "terms.hpp"

#include <hog/core/hash.hpp>

#include <algorithm>
#include <cmath>

namespace hog::globe::terms {

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

} // namespace hog::globe::terms
