#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>
#include <hog/physics/space.hpp>

#include <cstdint>

namespace hog::server {

struct MoveRules {
    double fastest = 6.6; // metres a second a player may cover, falling aside
    // Seconds of moving at the fastest that may arrive at once, as moves held up on the way do.
    double burst = 1.0;
    double sunk = 0.3; // metres the feet may be under the ground, allowing for rounding
};

enum class Verdict : std::uint8_t { Taken, TooFast, UnderGround };

// Where a player was last allowed to be, and how far they may move from there: they earn
// distance with time, up to a burst, and spend it moving anywhere but down.
class MoveCheck {
public:
    MoveCheck(WorldPosition at, MonotonicClock::time_point now, const MoveRules& rules = {});

    // Judges a move to `to`, against the ground in `space` where it holds any; if it's taken,
    // the player is there.
    Verdict judge(WorldPosition to, MonotonicClock::time_point now, const physics::Space& space);

    [[nodiscard]] WorldPosition at() const noexcept { return at_; }

private:
    MoveRules rules_;
    WorldPosition at_;
    MonotonicClock::time_point last_;
    double credit_; // metres
};

} // namespace hog::server
