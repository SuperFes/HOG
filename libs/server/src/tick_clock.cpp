#include <hog/core/assert.hpp>
#include <hog/server/tick_clock.hpp>

#include <chrono>

namespace hog::server {

namespace {

void check_rate(std::uint32_t rate) {
    HOG_ASSERT(rate > 0 && rate <= max_rate, "tick rate out of range");
}

void check_scale(std::int64_t scale) {
    HOG_ASSERT(scale > 0, "game time must move forward");
}

} // namespace

TickClock::TickClock(GameTime start, std::uint32_t rate, std::int64_t scale, Tick first)
    : tick_(first), since_tick_(first), since_time_(start), rate_(rate), scale_(scale) {
    check_rate(rate);
    check_scale(scale);
}

void TickClock::set_rate(std::uint32_t rate) {
    check_rate(rate);
    since_time_ = now();
    since_tick_ = tick_;
    rate_ = rate;
}

void TickClock::set_scale(std::int64_t scale) {
    check_scale(scale);
    since_time_ = now();
    since_tick_ = tick_;
    scale_ = scale;
}

GameTime TickClock::time_at(Tick tick) const noexcept {
    constexpr GameDuration::rep per_second = GameDuration{std::chrono::seconds{1}}.count();
    const GameDuration::rep ticks = (tick - since_tick_).count();
    return since_time_ + GameDuration{ticks * scale_ * per_second / rate_};
}

} // namespace hog::server
