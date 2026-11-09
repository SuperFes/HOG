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

    const double standing = floor + (away * valley.wall);
    Wall wall{.height = standing + rise,
              .gradient = DVec3{0.0},
              .floor = floor,
              .fall = DVec3{0.0},
              .reaching = 1.0 - (past * past * (3.0 - (2.0 * past))),
              .standing = standing};
    // Up the walls away from the line, and along its floor between its ends.
    const DVec3 outward = direction - (nearest * cosine);
    if (const double size = glm::length(outward); size > 0.0) {
        wall.fall += outward / size * valley.wall;
        wall.gradient += outward / size * (valley.wall + steepening);
    }
    const DVec3 ahead = along - (direction * glm::dot(along, direction));
    if (const double size = glm::length(ahead); size > 0.0 && t > 0.0 && t < 1.0) {
        const double run = std::sqrt(length) * radius;
        const DVec3 falling = ahead / size * ((valley.floor_to - valley.floor_from) / run);
        wall.gradient += falling;
        // The floor stops falling past its ends; water running off turns there smoothly.
        constexpr double turning = 0.1;
        const double end = std::min(t, 1.0 - t) / turning;
        const double kept = end >= 1.0 ? 1.0 : end * end * (3.0 - (2.0 * end));
        wall.fall += falling * kept;
    }
    return wall;
}

} // namespace hog::globe
