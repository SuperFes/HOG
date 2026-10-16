#include <hog/server/move_check.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>

namespace hog::server {

namespace {

// How far up from the feet the ground is looked for: well over any overhang a player stands
// under, so that buried feet meet the ground's underside first.
constexpr double looked_up = 1'000.0;

} // namespace

MoveCheck::MoveCheck(WorldPosition at, MonotonicClock::time_point now, const MoveRules& rules)
    : rules_(rules), at_(at), last_(now), credit_(rules.fastest * rules.burst) {}

Verdict MoveCheck::judge(WorldPosition to, MonotonicClock::time_point now,
                         const physics::Space& space) {
    const double seconds = std::chrono::duration<double>(now - last_).count();
    credit_ = std::min(rules_.fastest * rules_.burst,
                       credit_ + (rules_.fastest * std::max(seconds, 0.0)));
    last_ = now;

    const DVec3 up = glm::normalize(to.metres());
    const DVec3 moved = to - at_;
    const double cost = glm::length(moved - (up * std::min(glm::dot(moved, up), 0.0)));
    if (cost > credit_) {
        return Verdict::TooFast;
    }
    const auto above =
        space.cast_ray(to + (up * rules_.sunk), up * looked_up, physics::Faces::Both);
    if (above && above->back) {
        return Verdict::UnderGround;
    }
    credit_ -= cost;
    at_ = to;
    return Verdict::Taken;
}

} // namespace hog::server
