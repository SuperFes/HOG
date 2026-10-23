#pragma once

#include <hog/core/profile.hpp>
#include <hog/core/time.hpp>
#include <hog/server/catch_up.hpp>
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
    Tick first_tick{0};
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

// Runs the simulation one fixed tick at a time, paced to real time. While paused it keeps the
// pace but calls idle(clock) instead, leaving the clock where it is. Changes asked for from inside
// a step or idle take effect once it returns.
template <Timer T>
class TickLoop {
public:
    TickLoop(T& timer, const LoopConfig& config)
        : timer_(&timer), clock_(config.start, config.rate, config.time_scale, config.first_tick),
          max_burst_(config.max_burst) {}

    // Calls step(clock) once per tick until stop(); the clock shows the tick being run, and is
    // advanced after each call.
    template <typename Step>
        requires std::invocable<Step&, const TickClock&>
    void run(Step step) {
        run(step, [](const TickClock& /*unused*/) {});
    }
    template <typename Step, typename Idle>
        requires std::invocable<Step&, const TickClock&> && std::invocable<Idle&, const TickClock&>
    void run(Step step, Idle idle);

    // Safe from any thread and from inside a step; the loop finishes the tick it is running.
    void stop() noexcept { stopping_.store(true, std::memory_order_relaxed); }

    // Applies from the next tick on. Call it from the loop's thread.
    void set_rate(std::uint32_t rate) { pending_rate_ = rate; }
    // Applies from the next tick on. Call it from the loop's thread. Precondition: scale > 0.
    void set_scale(std::int64_t scale) { pending_scale_ = scale; }
    [[nodiscard]] std::int64_t scale() const noexcept {
        return pending_scale_.value_or(clock_.scale());
    }

    // Call these from the loop's thread.
    void pause() noexcept { paused_ = true; }
    void resume() noexcept { paused_ = false; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }
    // Runs `ticks` more back to back, paused or not.
    void step_ticks(std::uint64_t ticks) noexcept { owed_ += ticks; }
    // Runs ticks back to back, sped up as catch_up() does, until the clock reaches `target`.
    void catch_up_to(GameTime target, std::uint64_t budget) {
        pending_catch_up_ = CatchUpTo{.target = target, .budget = budget};
    }

    [[nodiscard]] const TickClock& clock() const noexcept { return clock_; }
    // Call it from the loop's thread.
    [[nodiscard]] const LoopStats& stats() const noexcept { return stats_; }

private:
    struct CatchUpTo {
        GameTime target;
        std::uint64_t budget = 1;
    };

    [[nodiscard]] bool stopping() const noexcept {
        return stopping_.load(std::memory_order_relaxed);
    }
    // Runs the step, timing it, without moving the clock.
    template <typename Step>
    void timed(Step& step);
    template <typename Step>
    void tick(Step& step);
    // Applies what a step or idle asked for; true if it ran ticks the pace didn't call for.
    template <typename Step>
    [[nodiscard]] bool apply(Step& step, Pacer& pacer);

    T* timer_;
    TickClock clock_;
    std::uint32_t max_burst_;
    std::optional<std::uint32_t> pending_rate_;
    std::optional<std::int64_t> pending_scale_;
    std::optional<CatchUpTo> pending_catch_up_;
    bool paused_ = false;
    std::uint64_t owed_ = 0;
    std::atomic<bool> stopping_{false};
    LoopStats stats_;
};

template <Timer T>
template <typename Step, typename Idle>
    requires std::invocable<Step&, const TickClock&> && std::invocable<Idle&, const TickClock&>
void TickLoop<T>::run(Step step, Idle idle) {
    Pacer pacer{timer_->now(), clock_.rate(), max_burst_};
    while (!stopping()) {
        std::uint32_t due = pacer.due(timer_->now());
        stats_.slipped = pacer.slipped();
        if (due == 0) {
            timer_->sleep_until(pacer.next_deadline());
            continue;
        }
        for (; due > 0 && !stopping(); --due) {
            if (paused_) {
                idle(std::as_const(clock_));
            } else {
                tick(step);
            }
            // Extra ticks took real time the pace shouldn't count as owed.
            if (apply(step, pacer)) {
                break;
            }
        }
    }
}

template <Timer T>
template <typename Step>
void TickLoop<T>::timed(Step& step) {
    const MonotonicClock::time_point started = timer_->now();
    {
        HOG_ZONE("tick");
        step(std::as_const(clock_));
    }
    HOG_FRAME();
    stats_.last_step = timer_->now() - started;
    stats_.longest_step = std::max(stats_.longest_step, stats_.last_step);
}

template <Timer T>
template <typename Step>
void TickLoop<T>::tick(Step& step) {
    timed(step);
    ++stats_.ticks;
    clock_.advance();
}

template <Timer T>
template <typename Step>
bool TickLoop<T>::apply(Step& step, Pacer& pacer) {
    if (pending_rate_) {
        clock_.set_rate(*pending_rate_);
        pacer.set_rate(*pending_rate_);
        pending_rate_.reset();
    }
    if (pending_scale_) {
        clock_.set_scale(*pending_scale_);
        pending_scale_.reset();
    }
    bool extra = false;
    while ((owed_ > 0 || pending_catch_up_) && !stopping()) {
        extra = true;
        if (owed_ > 0) {
            --owed_;
            tick(step);
        } else if (pending_catch_up_) {
            const CatchUpTo to = *pending_catch_up_;
            pending_catch_up_.reset();
            // catch_up moves clock_ itself.
            const CatchUpStats stats = catch_up(
                clock_, to.budget, [&to] { return to.target; },
                [this, &step](const TickClock& /*unused*/) { timed(step); });
            stats_.ticks += stats.ticks;
        }
        if (pending_scale_) {
            clock_.set_scale(*pending_scale_);
            pending_scale_.reset();
        }
    }
    if (extra) {
        pacer = Pacer{timer_->now(), clock_.rate(), max_burst_};
    }
    return extra;
}

} // namespace hog::server
