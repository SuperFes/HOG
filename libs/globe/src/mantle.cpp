#include "terms.hpp"

#include <algorithm>
#include <cmath>

namespace hog::globe::terms {
namespace {

// The cover feathers out against the bare rock over about this far, never wider than it is deep.
constexpr double contact_width = 1.0;

} // namespace

double smooth_max(double a, double b, double width) noexcept {
    const double h = std::max(width - std::abs(a - b), 0.0);
    return std::max(a, b) + (h * h / (4.0 * width));
}

double smooth_max_share(double a, double b, double width) noexcept {
    const double h = std::max(width - std::abs(a - b), 0.0);
    const double blend = h / (2.0 * width);
    return a >= b ? 1.0 - blend : blend;
}

double mantle(double rock, double smooth, double depth, double hold) noexcept {
    if (depth <= 0.0 || hold <= 0.0) {
        return 0.0;
    }
    const double width = std::min(contact_width, depth);
    return hold * (smooth_max(rock, smooth + depth, width) - rock);
}

} // namespace hog::globe::terms
