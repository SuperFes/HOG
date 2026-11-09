#include "valleys.hpp"

#include <algorithm>
#include <cmath>

namespace hog::globe {

Wall wall_at(const Valley& valley, DVec3 direction, double radius) noexcept {
    // Where along the line it's nearest, across its chord: lines are short against the planet.
    const DVec3 along = valley.to - valley.from;
    const double length = glm::dot(along, along);
    const double t = length > 0.0
                         ? std::clamp(glm::dot(direction - valley.from, along) / length, 0.0, 1.0)
                         : 0.0;
    const DVec3 nearest = glm::normalize(valley.from + (along * t));
    const double cosine = std::clamp(glm::dot(direction, nearest), -1.0, 1.0);
    const double away = std::acos(cosine) * radius;
    const double floor = valley.floor_from + ((valley.floor_to - valley.floor_from) * t);

    // Past half its reach, a wall rises away smoothly, out of the way by its reach.
    constexpr double out_of_the_way = 1e5;
    const double half = valley.reach / 2.0;
    const double past = std::clamp((away - half) / half, 0.0, 1.0);
    const double rise = out_of_the_way * past * past * (3.0 - (2.0 * past));
    const double steepening = out_of_the_way * 6.0 * past * (1.0 - past) / half;

    Wall wall{.height = floor + (away * valley.wall) + rise, .gradient = DVec3{0.0}};
    // Up the walls away from the line, and along its floor between its ends.
    const DVec3 outward = direction - (nearest * cosine);
    if (const double size = glm::length(outward); size > 0.0) {
        wall.gradient += outward / size * (valley.wall + steepening);
    }
    if (t > 0.0 && t < 1.0) {
        const DVec3 ahead = along - (direction * glm::dot(along, direction));
        if (const double size = glm::length(ahead); size > 0.0) {
            const double run = std::sqrt(length) * radius;
            wall.gradient += ahead / size * ((valley.floor_to - valley.floor_from) / run);
        }
    }
    return wall;
}

} // namespace hog::globe
