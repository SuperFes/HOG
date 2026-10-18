#pragma once

#include <hog/core/time.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace hog::client {

// The chat the player has heard, newest last.
class ChatLog {
public:
    // How many lines are kept to scroll back over, and how many stay over the picture while the
    // chat box is closed, each for how long.
    static constexpr std::size_t kept = 100;
    static constexpr std::size_t most_fresh = 8;
    static constexpr std::chrono::seconds fresh_for{10};

    struct Line {
        std::string from;
        std::string text;
        MonotonicClock::time_point at;
    };

    void add(std::string from, std::string text, MonotonicClock::time_point now);

    [[nodiscard]] std::span<const Line> lines() const noexcept { return lines_; }
    // The newest lines still shown while the chat box is closed.
    [[nodiscard]] std::span<const Line> fresh(MonotonicClock::time_point now) const noexcept;

private:
    std::vector<Line> lines_;
};

} // namespace hog::client
