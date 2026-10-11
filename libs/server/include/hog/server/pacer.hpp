#pragma once

#include <hog/core/time.hpp>

#include <cstdint>

namespace hog::server {

// Real-time deadlines for ticks at a fixed rate. A server that falls more than a burst behind, as
// after a stall, drops the excess from real time instead of running it back to back, so the world
// slows down rather than spiralling further behind.
class Pacer {
public:
    using TimePoint = MonotonicClock::time_point;

    // The first tick is due at `start`. Preconditions: 0 < rate <= max_rate, max_burst > 0.
    Pacer(TimePoint start, std::uint32_t rate, std::uint32_t max_burst);

    // The number of ticks due by `now`, at most max_burst; the caller is to run them all.
    [[nodiscard]] std::uint32_t due(TimePoint now);

    [[nodiscard]] TimePoint next_deadline() const noexcept { return deadline(taken_); }
    // Ticks dropped behind real time so far.
    [[nodiscard]] std::uint64_t slipped() const noexcept { return slipped_; }

    // Applies from the next deadline on. Precondition: 0 < rate <= max_rate.
    void set_rate(std::uint32_t rate);

private:
    [[nodiscard]] TimePoint deadline(std::uint64_t index) const noexcept;

    TimePoint start_;
    std::uint32_t rate_;
    std::uint32_t max_burst_;
    std::uint64_t taken_ = 0; // deadlines since start_ handed out or dropped
    std::uint64_t slipped_ = 0;
};

} // namespace hog::server
