#pragma once

#include <hog/core/assert.hpp>

#include <chrono>
#include <compare>
#include <cstdint>

namespace hog {

// Real time: monotonic for measuring frames and timeouts, wall clock only for logs and timestamps.
using MonotonicClock = std::chrono::steady_clock;
using WallClock = std::chrono::system_clock;

// In-world time. It has no now(): game time advances only as the simulation ticks, and is a
// distinct type so chrono rejects mixing it with real time.
struct GameClock {
    // NOLINTBEGIN(readability-identifier-naming): names required by the chrono clock interface
    using rep = std::int64_t;
    using period = std::micro;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<GameClock>;
    static constexpr bool is_steady = true;
    // NOLINTEND(readability-identifier-naming)
};

using GameDuration = GameClock::duration;
using GameTime = GameClock::time_point;

// A signed number of simulation steps.
class TickDelta {
public:
    constexpr TickDelta() noexcept = default;
    explicit constexpr TickDelta(std::int64_t count) noexcept : count_(count) {}

    [[nodiscard]] constexpr std::int64_t count() const noexcept { return count_; }

    constexpr auto operator<=>(const TickDelta&) const noexcept = default;
    constexpr TickDelta operator-() const noexcept { return TickDelta{-count_}; }
    friend constexpr TickDelta operator+(TickDelta a, TickDelta b) noexcept {
        return TickDelta{a.count_ + b.count_};
    }
    friend constexpr TickDelta operator-(TickDelta a, TickDelta b) noexcept {
        return TickDelta{a.count_ - b.count_};
    }

private:
    std::int64_t count_ = 0;
};

// A simulation step number, counted from zero when the world started.
class Tick {
public:
    constexpr Tick() noexcept = default;
    explicit constexpr Tick(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    constexpr auto operator<=>(const Tick&) const noexcept = default;

    constexpr Tick& operator++() noexcept {
        ++value_;
        return *this;
    }

    friend constexpr Tick operator+(Tick tick, TickDelta delta) noexcept {
        return Tick{tick.value_ + static_cast<std::uint64_t>(delta.count())};
    }
    friend constexpr Tick operator-(Tick tick, TickDelta delta) noexcept {
        return Tick{tick.value_ - static_cast<std::uint64_t>(delta.count())};
    }
    friend constexpr TickDelta operator-(Tick a, Tick b) noexcept {
        return TickDelta{static_cast<std::int64_t>(a.value_ - b.value_)};
    }

private:
    std::uint64_t value_ = 0;
};

// Maps ticks to game time with integer arithmetic only, so every machine agrees exactly.
class TickSchedule {
public:
    // Precondition: per_tick > 0.
    TickSchedule(GameTime epoch, GameDuration per_tick) : epoch_(epoch), per_tick_(per_tick) {
        HOG_ASSERT(per_tick.count() > 0, "a tick must advance game time");
    }

    [[nodiscard]] GameTime epoch() const noexcept { return epoch_; }
    [[nodiscard]] GameDuration per_tick() const noexcept { return per_tick_; }

    [[nodiscard]] GameTime time_at(Tick tick) const noexcept {
        return epoch_ + per_tick_ * static_cast<GameDuration::rep>(tick.value());
    }

    // The last tick that starts at or before `time`. Precondition: time >= epoch.
    [[nodiscard]] Tick tick_at(GameTime time) const {
        HOG_ASSERT(time >= epoch_, "time is before the schedule's epoch");
        return Tick{static_cast<std::uint64_t>((time - epoch_) / per_tick_)};
    }

private:
    GameTime epoch_;
    GameDuration per_tick_;
};

} // namespace hog
