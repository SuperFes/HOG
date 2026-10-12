#pragma once

#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>

#include <cstdint>

namespace hog::sim::detail {

// The last whole multiple of `step` since the epoch at or before `now`.
[[nodiscard]] inline GameTime last_step(GameTime now, GameDuration step) {
    const auto since = now.time_since_epoch();
    auto steps = since / step;
    if (since % step < GameDuration::zero()) {
        --steps;
    }
    return GameTime{steps * step};
}

// Runs body(index) for every cell, across the jobs when given.
template <typename F>
void for_cells(jobs::JobSystem* jobs, std::uint32_t count, F body) {
    constexpr std::uint32_t cells_per_job = 256;
    if (jobs == nullptr) {
        for (std::uint32_t index = 0; index < count; ++index) {
            body(index);
        }
        return;
    }
    jobs->parallel_for(count, cells_per_job, [&](jobs::Range range, std::uint32_t /*thread*/) {
        for (std::uint32_t index = range.begin; index < range.end; ++index) {
            body(index);
        }
    });
}

} // namespace hog::sim::detail
