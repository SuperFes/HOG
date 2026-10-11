#pragma once

#include <hog/core/assert.hpp>
#include <hog/core/time.hpp>
#include <hog/server/tick_clock.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace hog::server {

struct CatchUpStats {
    std::uint64_t ticks = 0;
    std::int64_t largest_scale = 0;
};

// Runs step(clock) back to back until the clock is less than one live tick behind now(), since
// planet time is locked to real time and a resumed world must return to it. Each pass raises the
// time scale so the remaining gap takes at most about `budget` ticks, then re-reads now() to cover
// the time the pass itself took; the clock is left at its live scale. Precondition: budget > 0.
template <typename Now, typename Step>
    requires std::same_as<std::invoke_result_t<Now&>, GameTime> &&
             std::invocable<Step&, const TickClock&>
CatchUpStats catch_up(TickClock& clock, std::uint64_t budget, Now now, Step step) {
    HOG_ASSERT(budget > 0, "catching up needs at least one tick per pass");
    const std::int64_t live_scale = clock.scale();
    CatchUpStats stats{.ticks = 0, .largest_scale = live_scale};
    for (;;) {
        const GameTime target = now();
        const GameDuration live_step = clock.step();
        const GameDuration gap = target - clock.now();
        if (gap < live_step) {
            return stats;
        }
        const auto per_pass = live_step * static_cast<GameDuration::rep>(budget);
        const std::int64_t factor = (gap + per_pass - GameDuration{1}) / per_pass;
        const std::int64_t scale = live_scale * std::max<std::int64_t>(factor, 1);
        stats.largest_scale = std::max(stats.largest_scale, scale);
        clock.set_scale(scale);
        while (clock.now() + clock.step() <= target) {
            step(std::as_const(clock));
            clock.advance();
            ++stats.ticks;
        }
        clock.set_scale(live_scale);
    }
}

} // namespace hog::server
