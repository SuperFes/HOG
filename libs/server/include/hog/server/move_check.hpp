#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>
#include <hog/physics/space.hpp>

#include <cstdint>
#include <functional>
#include <optional>

namespace hog::server {

struct MoveRules {
    double fastest = 6.6; // metres a second a player may cover, falling aside
    // Seconds of moving at the fastest that may arrive at once, as moves held up on the way do.
    double burst = 1.0;
    double sunk = 0.3; // metres the feet may be under the ground, allowing for rounding
    double steepest = 0.87; // radians from level climbed where the ground's grip isn't known
    // Metres of rise beyond what the slope holds that may come at once, a jump's worth and a
    // step's, and how fast it comes back.
    double lift = 1.6;
    double lift_regained = 1.2; // metres a second
};

// The steepest slope, in radians from level, held at a place; none where that isn't known.
using Grip = std::function<std::optional<double>(WorldPosition)>;

enum class Verdict : std::uint8_t { Taken, TooFast, UnderGround, TooSteep };

// Where a player was last allowed to be, and how far they may move from there: they earn
// distance with time, up to a burst, and spend it moving anywhere but down. Rising faster than
// the slope under them holds costs from a smaller allowance, which is what a jump spends.
class MoveCheck {
public:
    MoveCheck(WorldPosition at, MonotonicClock::time_point now, const MoveRules& rules = {});

    // Judges a move to `to`, against the ground in `space` where it holds any and the slope
    // `grip` says is held; if it's taken, the player is there.
    Verdict judge(WorldPosition to, MonotonicClock::time_point now, const physics::Space& space,
                  const Grip& grip = {});

    [[nodiscard]] WorldPosition at() const noexcept { return at_; }

private:
    MoveRules rules_;
    WorldPosition at_;
    MonotonicClock::time_point last_;
    double credit_; // metres
    double lift_;   // metres
};

} // namespace hog::server
