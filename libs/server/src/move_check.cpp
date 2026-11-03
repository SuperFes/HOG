#include <hog/server/move_check.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>

namespace hog::server {

namespace {

// How far up from the feet the ground is looked for: well over any overhang a player stands
// under, so that buried feet meet the ground's underside first.
constexpr double looked_up = 1'000.0;

} // namespace

MoveCheck::MoveCheck(WorldPosition at, MonotonicClock::time_point now, const MoveRules& rules)
    : rules_(rules), at_(at), last_(now), credit_(rules.fastest * rules.burst), lift_(rules.lift) {}

Verdict MoveCheck::judge(WorldPosition to, MonotonicClock::time_point now,
                         const physics::Space& space, const Grip& grip) {
    const double seconds = std::chrono::duration<double>(now - last_).count();
    credit_ = std::min(rules_.fastest * rules_.burst,
                       credit_ + (rules_.fastest * std::max(seconds, 0.0)));
    lift_ = std::min(rules_.lift, lift_ + (rules_.lift_regained * std::max(seconds, 0.0)));
    last_ = now;

    const DVec3 up = glm::normalize(to.metres());
    const DVec3 moved = to - at_;
    const double cost = glm::length(moved - (up * std::min(glm::dot(moved, up), 0.0)));
    if (cost > credit_) {
        return Verdict::TooFast;
    }
    // Rise past what the slope holds, over the ground under the feet they leave.
    const DVec3 sideways = moved - (up * glm::dot(moved, up));
    const double rise = glm::dot(moved, up);
    double excess = 0.0;
    if (rise > 0.0) {
        const double held = grip ? grip(at_).value_or(rules_.steepest) : rules_.steepest;
        excess = std::max(rise - (glm::length(sideways) * std::tan(std::min(held, rules_.steepest))),
                          0.0);
        if (excess > lift_) {
            return Verdict::TooSteep;
        }
    }
    const auto above =
        space.cast_ray(to + (up * rules_.sunk), up * looked_up, physics::Faces::Both);
    if (above && above->back) {
        return Verdict::UnderGround;
    }
    credit_ -= cost;
    lift_ -= excess;
    at_ = to;
    return Verdict::Taken;
}

} // namespace hog::server
