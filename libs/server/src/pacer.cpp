#include <hog/core/assert.hpp>
#include <hog/server/pacer.hpp>
#include <hog/server/tick_clock.hpp>

#include <algorithm>
#include <chrono>

namespace hog::server {

namespace {

void check_rate(std::uint32_t rate) {
    HOG_ASSERT(rate > 0 && rate <= max_rate, "tick rate out of range");
}

} // namespace

Pacer::Pacer(TimePoint start, std::uint32_t rate, std::uint32_t max_burst)
    : start_(start), rate_(rate), max_burst_(max_burst) {
    check_rate(rate);
    HOG_ASSERT(max_burst > 0, "a pacer must let ticks run");
}

std::uint32_t Pacer::due(TimePoint now) {
    if (now < deadline(taken_)) {
        return 0;
    }
    // The last deadline passed: deadline() rounds down, so this estimate is never past it and at
    // most a step short.
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_);
    const auto seconds = static_cast<std::uint64_t>(elapsed / std::chrono::seconds{1});
    const auto fraction = static_cast<std::uint64_t>((elapsed % std::chrono::seconds{1}).count());
    std::uint64_t last = (seconds * rate_) + (fraction * rate_ / 1'000'000'000U);
    while (deadline(last + 1) <= now) {
        ++last;
    }
    const std::uint64_t behind = last + 1 - taken_;
    const std::uint64_t run = std::min<std::uint64_t>(behind, max_burst_);
    slipped_ += behind - run;
    taken_ = last + 1;
    return static_cast<std::uint32_t>(run);
}

void Pacer::set_rate(std::uint32_t rate) {
    check_rate(rate);
    start_ = next_deadline();
    taken_ = 0;
    rate_ = rate;
}

Pacer::TimePoint Pacer::deadline(std::uint64_t index) const noexcept {
    const std::chrono::nanoseconds offset =
        std::chrono::seconds{index / rate_} +
        std::chrono::nanoseconds{(index % rate_) * 1'000'000'000U / rate_};
    return start_ + std::chrono::duration_cast<TimePoint::duration>(offset);
}

} // namespace hog::server
