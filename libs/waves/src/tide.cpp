#include <hog/waves/tide.hpp>

#include <cmath>

namespace hog::waves {

double tide_at(DVec3 up, std::span<const Pull> pulls) noexcept {
    const DVec3 unit = glm::normalize(up);
    const double longitude = std::atan2(unit.y, unit.x);
    const double cos_latitude = std::sqrt(1.0 - (unit.z * unit.z));
    // The shape of the coasts makes it higher in some seas than others, and later.
    const double shaped = 1.0 + (0.5 * std::sin((3.0 * unit.x) + (2.0 * unit.z)));
    const double later = 1.5 * std::sin((2.0 * unit.y) - unit.z);

    double tide = 0.0;
    for (const Pull& pull : pulls) {
        const DVec3 toward = glm::normalize(pull.toward);
        const double hour = longitude - std::atan2(toward.y, toward.x);
        const double cos_declination = std::sqrt(1.0 - (toward.z * toward.z));
        // The two parts of the equilibrium tide that come round with the day: twice, highest
        // under the body's path and none at the poles; and once, where the body stands off the
        // equator, none on it.
        const double twice = cos_latitude * cos_latitude * cos_declination * cos_declination *
                             std::cos((2.0 * hour) - later);
        const double once = 4.0 * unit.z * cos_latitude * toward.z * cos_declination *
                            std::cos(hour - (later / 2.0));
        tide += pull.height * (twice + once);
    }
    return shaped * tide;
}

} // namespace hog::waves
