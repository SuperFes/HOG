#include <hog/waves/run_up.hpp>
#include <hog/waves/waves.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::waves {

RunUp run_up(double height, double period, double slope) noexcept {
    if (!(height > 0.0) || !(period > 0.0)) {
        return {};
    }
    const double beach = std::clamp(slope, gentlest_beach, steepest_beach);
    // The root of the height times the deep-water wavelength.
    const double scale = std::sqrt(height * gravity * period * period / (2.0 * std::numbers::pi));
    return {.setup = 0.35 * beach * scale,
            .incident = scale * std::sqrt(0.563) * beach,
            .infragravity = scale * std::sqrt(0.004)};
}

double run_up_reach(const RunUp& run) noexcept {
    return 1.1 * (run.setup + (std::hypot(run.incident, run.infragravity) / 2.0));
}

} // namespace hog::waves
