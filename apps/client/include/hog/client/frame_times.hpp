#pragma once

#include <hog/render/gpu_timer.hpp>

#include <cstddef>
#include <deque>
#include <span>
#include <string_view>
#include <vector>

namespace hog::client {

// How long something took over the frames held, in milliseconds.
struct Timing {
    std::string_view name;
    double mean = 0.0;
    double worst = 0.0;
};

// The last second or so of frames: how long each took the CPU, from one to the next, and the GPU
// over each of its passes.
class FrameTimes {
public:
    explicit FrameTimes(double seconds = 1.0) noexcept : seconds_{seconds} {}

    // A frame of `seconds`, with the GPU's spans of a frame lately finished, whose names must
    // outlast these. Frames past the last `seconds` held go, though the newest stays.
    void add(double seconds, std::span<const render::GpuSpan> gpu);

    [[nodiscard]] std::size_t frames() const noexcept { return frames_.size(); }
    // Named "frame".
    [[nodiscard]] Timing cpu() const noexcept;
    // Each pass the GPU timed, in the order the newest frame timing them did, each over the frames
    // that timed it; then all of them together, as "GPU". Empty where none were timed.
    [[nodiscard]] std::vector<Timing> gpu() const;
    // Each frame's milliseconds, oldest first.
    [[nodiscard]] std::vector<float> history() const;

private:
    struct Held {
        double milliseconds = 0.0;
        std::vector<render::GpuSpan> gpu;
    };

    std::deque<Held> frames_;
    double seconds_;
    double held_ = 0.0; // seconds
};

} // namespace hog::client
