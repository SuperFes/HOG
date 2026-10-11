#pragma once

#include <hog/core/time.hpp>

#include <cstdint>

namespace hog::server {

// Tick rates in hertz.
inline constexpr std::uint32_t idle_rate = 20;
inline constexpr std::uint32_t combat_rate = 30;
inline constexpr std::uint32_t max_rate = 60;

// Live worlds run at real time; faster scales are for offline runs.
inline constexpr std::int64_t default_time_scale = 1;

// The current tick and the game time it starts at. Game time runs `scale` times faster than real
// time whatever the tick rate, in integer arithmetic so a replay reaches exactly the same times.
class TickClock {
public:
    // Preconditions: 0 < rate <= max_rate, scale > 0.
    TickClock(GameTime start, std::uint32_t rate, std::int64_t scale);

    [[nodiscard]] Tick tick() const noexcept { return tick_; }
    [[nodiscard]] GameTime now() const noexcept { return time_at(tick_); }
    // The game time the current tick covers.
    [[nodiscard]] GameDuration step() const noexcept {
        return time_at(tick_ + TickDelta{1}) - now();
    }
    [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }
    [[nodiscard]] std::int64_t scale() const noexcept { return scale_; }

    void advance() noexcept { ++tick_; }

    // Applies from the current tick on. Precondition: 0 < rate <= max_rate.
    void set_rate(std::uint32_t rate);

private:
    [[nodiscard]] GameTime time_at(Tick tick) const noexcept;

    Tick tick_;
    // The tick and time the current rate took effect, so a change never moves earlier times.
    Tick since_tick_;
    GameTime since_time_;
    std::uint32_t rate_;
    std::int64_t scale_;
};

} // namespace hog::server
