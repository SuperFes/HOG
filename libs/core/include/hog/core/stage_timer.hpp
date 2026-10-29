#pragma once

#include <hog/core/time.hpp>

#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hog {

// How long each stage of a long job took, added up over every time it ran.
class StageTimer {
public:
    using Now = std::function<MonotonicClock::time_point()>;

    struct Stage {
        std::string name;
        std::chrono::nanoseconds took{};
        int runs = 0;
    };

    explicit StageTimer(Now now = &MonotonicClock::now) : now_{std::move(now)} {}

    // Runs `stage`, adding the time it takes to `name`'s, and hands back what it returns.
    template <typename Run>
    decltype(auto) time(std::string_view name, Run&& run) {
        const Timing timing{*this, name};
        return std::forward<Run>(run)();
    }

    // In the order they first ran.
    [[nodiscard]] std::span<const Stage> stages() const noexcept { return stages_; }

    // A line a stage: its name, its seconds, and how many runs where more than one; then the total.
    [[nodiscard]] std::string report() const;

private:
    class Timing {
    public:
        Timing(StageTimer& timer, std::string_view name)
            : timer_{timer}, name_{name}, start_{timer.now_()} {}
        Timing(const Timing&) = delete;
        Timing(Timing&&) = delete;
        Timing& operator=(const Timing&) = delete;
        Timing& operator=(Timing&&) = delete;
        ~Timing() { timer_.add(name_, timer_.now_() - start_); }

    private:
        StageTimer& timer_;
        std::string_view name_;
        MonotonicClock::time_point start_;
    };

    void add(std::string_view name, std::chrono::nanoseconds took);

    Now now_;
    std::vector<Stage> stages_;
};

} // namespace hog
