#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/replication/states.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string_view>
#include <vector>

namespace hog::replication {

struct TimelineSettings {
    // How far behind the snapshots others are shown, so there's usually one either side.
    std::chrono::milliseconds delay{100};
    // How far past the newest snapshot others carry on as they were going when the next is late.
    std::chrono::milliseconds reach{100};
};

// An entity where it's shown.
struct Shown {
    protocol::NetId id{};
    WorldPosition at;
    std::string_view name; // until the timeline is next added to
};

// The snapshots a client has applied, laid out in time to show others moving smoothly a little
// in the past. Each is timed by its tick, on a clock fitted to the quickest of the recent
// arrivals, so snapshots that come late or bunched together don't show as stops and jumps.
class Timeline {
public:
    // Snapshots kept to show from: 400 ms of them.
    static constexpr std::size_t kept = 8;
    // Arrivals the clock is fitted to: a second of them.
    static constexpr std::size_t fitted = 20;

    explicit Timeline(const TimelineSettings& settings = {}) noexcept : settings_(settings) {}

    // Ignores a tick no newer than the last added.
    void add(std::uint32_t tick, MonotonicClock::time_point arrived, EntityStates states);

    // In order of id; nothing before the first snapshot.
    [[nodiscard]] std::vector<Shown> at(MonotonicClock::time_point now) const;

private:
    struct Held {
        std::uint32_t tick = 0;
        EntityStates states;
    };

    TimelineSettings settings_;
    // Oldest first.
    std::deque<Held> held_;
    // When tick 0 would have come, by each recent arrival.
    std::deque<MonotonicClock::time_point> starts_;
};

} // namespace hog::replication
