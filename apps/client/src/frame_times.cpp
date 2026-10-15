#include <hog/client/frame_times.hpp>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace hog::client {

namespace {

// Of each pass, the frames timing it, their sum and the worst.
struct Sum {
    std::string_view name;
    int frames = 0;
    double total = 0.0;
    double worst = 0.0;

    void add(double milliseconds) noexcept {
        ++frames;
        total += milliseconds;
        worst = std::max(worst, milliseconds);
    }
    [[nodiscard]] Timing timing() const noexcept {
        return {.name = name, .mean = total / frames, .worst = worst};
    }
};

} // namespace

void FrameTimes::add(double seconds, std::span<const render::GpuSpan> gpu) {
    frames_.push_back({.milliseconds = seconds * 1'000.0, .gpu = {gpu.begin(), gpu.end()}});
    held_ += seconds;
    while (frames_.size() > 1 && held_ > seconds_) {
        held_ -= frames_.front().milliseconds / 1'000.0;
        frames_.pop_front();
    }
}

Timing FrameTimes::cpu() const noexcept {
    Sum sum{.name = "frame"};
    for (const Held& frame : frames_) {
        sum.add(frame.milliseconds);
    }
    return sum.frames == 0 ? Timing{.name = sum.name} : sum.timing();
}

std::vector<Timing> FrameTimes::gpu() const {
    std::vector<Sum> passes;
    Sum all{.name = "GPU"};
    // Newest first, so passes are in the order it timed them.
    for (const Held& frame : std::views::reverse(frames_)) {
        if (frame.gpu.empty()) {
            continue;
        }
        double together = 0.0;
        for (const render::GpuSpan& span : frame.gpu) {
            auto pass = std::ranges::find(passes, span.name, &Sum::name);
            if (pass == passes.end()) {
                passes.push_back({.name = span.name});
                pass = std::prev(passes.end());
            }
            pass->add(span.milliseconds);
            together += span.milliseconds;
        }
        all.add(together);
    }
    std::vector<Timing> timings;
    if (all.frames == 0) {
        return timings;
    }
    for (const Sum& pass : passes) {
        timings.push_back(pass.timing());
    }
    timings.push_back(all.timing());
    return timings;
}

std::vector<float> FrameTimes::history() const {
    std::vector<float> history;
    history.reserve(frames_.size());
    for (const Held& frame : frames_) {
        history.push_back(static_cast<float>(frame.milliseconds));
    }
    return history;
}

} // namespace hog::client
