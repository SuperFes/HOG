#include <hog/terrain/bend.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::terrain {

namespace {

// Moves a point from a sphere of radius `from` to one of radius `to`, touching under the eye. Each
// step keeps its precision relative to the point's offset from the eye, as floats need it to.
DVec3 rebend(const Bend& bend, double from, double to, DVec3 from_eye) {
    if (from == to) {
        return from_eye;
    }
    const double along_up = glm::dot(from_eye, bend.up);
    const DVec3 across = from_eye - (bend.up * along_up);
    const double out = glm::length(across);
    // Over the point where the spheres touch.
    const double above = along_up + bend.eye_height;
    const double turned = std::atan2(out, from + above);
    const double height = ((2.0 * from * above) + (above * above) + (out * out)) /
                          (std::hypot(from + above, out) + from);

    const double turning = turned * from / to;
    const double half = std::sin(turning / 2.0);
    const double lifted = (height * std::cos(turning)) - (2.0 * to * half * half);
    const double spread = (to + height) * std::sin(turning);
    return (bend.up * (lifted - bend.eye_height)) +
           (out > 0.0 ? across * (spread / out) : DVec3{0.0});
}

} // namespace

double blended_out(double radius, const BendSettings& settings) noexcept {
    // Seen from h up, a sphere of radius F has its horizon √(2·F·h) away, and the far side lies a
    // quarter turn, π·r/2, round the true one. With the curvature blending as h², that holds at
    // every height while the blend's done within about 2·√(r³/F); a little sooner, for room.
    constexpr double sooner = 1.5;
    return std::min(settings.blended_out,
                    sooner * std::sqrt(radius * radius * radius / settings.faked_radius));
}

double faked_radius(double radius, double over_ground, const BendSettings& settings) {
    const double blend = std::clamp(over_ground / blended_out(radius, settings), 0.0, 1.0);
    if (radius >= settings.faked_radius || blend == 1.0) {
        return radius;
    }
    const double smooth = blend * blend * (3.0 - (2.0 * blend));
    // Curvature rather than radius blends, keeping the horizon near while it does.
    return 1.0 / std::lerp(1.0 / settings.faked_radius, 1.0 / radius, smooth);
}

Bend bend_from(WorldPosition eye, double radius, double ground, const BendSettings& settings) {
    const double from_centre = glm::length(eye.metres());
    const double eye_height = from_centre - radius;
    return {.up = eye.metres() / from_centre,
            .eye_height = eye_height,
            .radius = radius,
            .faked = faked_radius(radius, std::max(eye_height - ground, 0.0), settings)};
}

DVec3 bent(const Bend& bend, DVec3 from_eye) {
    return rebend(bend, bend.radius, bend.faked, from_eye);
}

bool behind_horizon(const Bend& bend, DVec3 from_eye, double reach) {
    const double out = bend.faked + bend.eye_height;
    if (bend.eye_height <= 0.0) {
        return false;
    }
    // Within the cone the sphere fills, every sight line meets it no further than its horizon.
    const DVec3 drawn = bent(bend, from_eye);
    const double away = glm::length(drawn);
    const double horizon = std::sqrt((out * out) - (bend.faked * bend.faked));
    if (away - reach <= horizon) {
        return false;
    }
    const double filled = std::asin(bend.faked / out);
    const double off = std::acos(std::clamp(glm::dot(drawn / away, -bend.up), -1.0, 1.0));
    return off + std::asin(reach / away) < filled;
}

DVec3 unbent(const Bend& bend, DVec3 from_eye) {
    return rebend(bend, bend.faked, bend.radius, from_eye);
}

bool folds_over(const Bend& bend, DVec3 middle, double reach) {
    if (bend.faked == bend.radius) {
        return false;
    }
    const double out = glm::length(middle);
    if (reach >= out) {
        return true;
    }
    const double away = std::acos(std::clamp(glm::dot(middle / out, bend.up), -1.0, 1.0));
    return away + std::asin(reach / out) >= std::numbers::pi;
}

} // namespace hog::terrain
