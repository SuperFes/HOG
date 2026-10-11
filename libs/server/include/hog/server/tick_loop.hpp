#pragma once

#include <hog/core/time.hpp>
#include <hog/server/pacer.hpp>
#include <hog/server/tick_clock.hpp>

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

namespace hog::server {

// Where the loop reads real time and waits for it; tests substitute a clock they move by hand.
template <typename T>
concept Timer = requires(T& timer, MonotonicClock::time_point when) {
    { timer.now() } -> std::same_as<MonotonicClock::time_point>;
    timer.sleep_until(when);
};

struct SteadyTimer {
    [[nodiscard]] static MonotonicClock::time_point now() { return MonotonicClock::now(); }
    static void sleep_until(MonotonicClock::time_point when) {
        std::this_thread::sleep_until(when);
    }
};

struct LoopConfig {
    GameTime start{GameDuration::zero()};
    std::uint32_t rate = idle_rate;
    std::int64_t time_scale = default_time_scale;
    // Ticks run back to back to catch up before the rest are dropped.
    std::uint32_t max_burst = 5;
};

struct LoopStats {
    std::uint64_t ticks = 0;
    std::uint64_t slipped = 0; // ticks dropped behind real time
    MonotonicClock::duration last_step{};
    MonotonicClock::duration longest_step{};
};

// Runs the simulation one fixed tick at a time, paced to real time.
template <Timer T>
class TickLoop {
public:
    TickLoop(T& timer, const LoopConfig& config)
        : timer_(&timer), clock_(config.start, config.rate, config.time_scale),
          max_burst_(config.max_burst) {}

    // Calls step(clock) once per tick until stop(); the clock shows the tick being run, and is
    // advanced after each call.
    template <typename Step>
        requires std::invocable<Step&, const TickClock&>
    void run(Step step);

    // Safe from any thread and from inside a step; the loop finishes the tick it is running.
    void stop() noexcept { stopping_.store(true, std::memory_order_relaxed); }

    // Applies from the next tick on. Call it from the loop's thread.
    void set_rate(std::uint32_t rate) { pending_rate_ = rate; }

    [[nodiscard]] const TickClock& clock() const noexcept { return clock_; }
    // Call it from the loop's thread.
    [[nodiscard]] const LoopStats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] bool stopping() const noexcept {
        return stopping_.load(std::memory_order_relaxed);
    }

    T* timer_;
    TickClock clock_;
    std::uint32_t max_burst_;
    std::optional<std::uint32_t> pending_rate_;
    std::atomic<bool> stopping_{false};
    LoopStats stats_;
};

template <Timer T>
template <typename Step>
    requires std::invocable<Step&, const TickClock&>
void TickLoop<T>::run(Step step) {
    Pacer pacer{timer_->now(), clock_.rate(), max_burst_};
    while (!stopping()) {
        std::uint32_t due = pacer.due(timer_->now());
        stats_.slipped = pacer.slipped();
        if (due == 0) {
            timer_->sleep_until(pacer.next_deadline());
            continue;
        }
        for (; due > 0 && !stopping(); --due) {
            const MonotonicClock::time_point started = timer_->now();
            step(std::as_const(clock_));
            stats_.last_step = timer_->now() - started;
            stats_.longest_step = std::max(stats_.longest_step, stats_.last_step);
            ++stats_.ticks;
            clock_.advance();
            if (pending_rate_) {
                clock_.set_rate(*pending_rate_);
                pacer.set_rate(*pending_rate_);
                pending_rate_.reset();
            }
        }
    }
}

} // namespace hog::server
