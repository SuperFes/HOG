#include "valleys.hpp"

#include <hog/core/portable_maths.hpp>
#include <hog/globe/noise.hpp>

#include <algorithm>
#include <cmath>

namespace hog::globe {

namespace {

double smoothstep(double x) noexcept {
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

} // namespace

double rise_past(double away, double reach) noexcept {
    const double half = reach / 2.0;
    return out_of_the_way * smoothstep((away - half) / half);
}

Wall wall_at(const Valley& valley, DVec3 direction, double radius) noexcept {
    // Where along the line it's nearest, across its chord: lines are short against the planet.
    const DVec3 along = valley.to - valley.from;
    const double length = glm::dot(along, along);
    const double t = length > 0.0
                         ? std::clamp(glm::dot(direction - valley.from, along) / length, 0.0, 1.0)
                         : 0.0;
    const DVec3 nearest = glm::normalize(valley.from + (along * t));
    const double cosine = std::clamp(glm::dot(direction, nearest), -1.0, 1.0);
    const double away = maths::atan2(glm::length(glm::cross(direction, nearest)), cosine) * radius;
    const double floor = valley.floor_from + ((valley.floor_to - valley.floor_from) * t);

    const double half = valley.reach / 2.0;
    const double past = std::clamp((away - half) / half, 0.0, 1.0);
    const double rise = rise_past(away, valley.reach);
    const double steepening = out_of_the_way * 6.0 * past * (1.0 - past) / half;

    // Up the walls from the belt's edge, their foot rounded over half the belt where it's worn
    // down to the floor (or the channel's width, where the belt is narrower), never past the
    // middle of the floor, which lies level across.
    const double out = away - valley.belt;
    const double toe = std::min(std::max(0.5 * valley.belt, valley.channel_width), valley.belt);
    double climbed = 0.0;
    double climbing = 0.0;
    if (toe <= 0.0 || out >= toe) {
        climbed = valley.wall * std::max(out, 0.0);
        climbing = out > 0.0 ? valley.wall : 0.0;
    } else if (out > -toe) {
        climbed = valley.wall * (out + toe) * (out + toe) / (4.0 * toe);
        climbing = valley.wall * (out + toe) / (2.0 * toe);
    }

    // The river winds along the floor, but runs into the next valley's where they meet.
    const DVec3 square = glm::cross(valley.from, valley.to);
    const double side = glm::dot(direction, square) < 0.0 ? -1.0 : 1.0;
    const double run = (valley.head + ((valley.foot - valley.head) * t)) * valley.length;
    double cut = 0.0;
    double cutting = 0.0;
    if (valley.channel_width > 0.0 && away < valley.belt + toe) {
        const double ends = smoothstep(run / valley.meander_length) *
                            smoothstep((valley.length - run) / valley.meander_length);
        const Octaves bends{.wavelength = valley.meander_length,
                            .gain = 0.5,
                            .finest = valley.meander_length / 4.0};
        const double wound =
            valley.meander * ends * fbm(DVec3{run, 0.0, 0.0}, bends, 0.0, valley.seed).value;
        const double off = (side * away) - wound;
        const double across = 2.0 * off / valley.channel_width;
        if (std::abs(across) < 1.0) {
            cut = valley.channel_depth * (1.0 - (across * across));
            cutting = -valley.channel_depth * 4.0 * across / valley.channel_width;
        }
    }

    const double standing = floor + climbed - cut;
    Wall wall{.height = standing + rise,
              .gradient = DVec3{0.0},
              .floor = floor - cut,
              .fall = DVec3{0.0},
              .reaching = 1.0 - (past * past * (3.0 - (2.0 * past))),
              .standing = standing};
    // Up the walls away from the line, and along its floor between its ends.
    const DVec3 outward = direction - (nearest * cosine);
    if (const double size = glm::length(outward); size > 0.0) {
        const DVec3 unit = outward / size;
        wall.fall += unit * climbing;
        wall.standing_gradient += unit * (climbing - (side * cutting));
        wall.gradient += unit * (climbing + steepening - (side * cutting));
        wall.reaching_gradient = unit * (-steepening / out_of_the_way);
    }
    // The way the floor runs, turning smoothly from one bend to the next.
    DVec3 runs = along;
    if (glm::dot(valley.runs_from, valley.runs_from) > 0.0) {
        runs = (valley.runs_from * (1.0 - t)) + (valley.runs_to * t);
        runs *= std::sqrt(length) / std::max(glm::length(runs), 1e-300);
    }
    const DVec3 ahead = runs - (direction * glm::dot(runs, direction));
    const bool inside = (t > 0.0 || valley.head > 0.0) && (t < 1.0 || valley.foot < 1.0);
    if (const double size = glm::length(ahead); size > 0.0 && inside) {
        const double span = std::sqrt(length) * radius;
        const DVec3 falling = ahead / size * ((valley.floor_to - valley.floor_from) / span);
        wall.gradient += falling;
        wall.standing_gradient += falling;
        // The floor stops falling past the valley's ends; water running off turns there
        // smoothly.
        constexpr double turning = 0.1;
        const double whole = valley.head + ((valley.foot - valley.head) * t);
        const double end = std::min(whole, 1.0 - whole) / turning;
        const double kept = end >= 1.0 ? 1.0 : end * end * (3.0 - (2.0 * end));
        wall.fall += falling * kept;
    }
    return wall;
}

} // namespace hog::globe
