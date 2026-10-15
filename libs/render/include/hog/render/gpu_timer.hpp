#pragma once

#include <hog/core/result.hpp>
#include <hog/render/vulkan.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace hog::render {

class Device;

// How long the GPU spent on part of a frame.
struct GpuSpan {
    std::string_view name;
    double milliseconds = 0.0;
};

// Times spans of the frames in flight with timestamp queries, each read back once its frame's
// submit has finished, so the CPU never waits on the GPU for them. Where the queue can't time, it
// records and reads nothing.
class GpuTimer {
public:
    static constexpr std::uint32_t most_spans = 16; // a frame

    [[nodiscard]] static Result<GpuTimer> create(const Device& device, std::uint32_t frames);

    GpuTimer() = default;
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;
    GpuTimer(GpuTimer&& other) noexcept;
    GpuTimer& operator=(GpuTimer&& other) noexcept;
    ~GpuTimer();

    [[nodiscard]] bool timing() const noexcept { return pool_ != VK_NULL_HANDLE; }

    // Collects the spans the slot's last frame timed, and records readying it for this one into
    // `commands`, outside any rendering and before its first span. The slot's last submit must
    // have finished. What's returned lasts until the slot is read again.
    std::span<const GpuSpan> read(VkCommandBuffer commands, std::uint32_t slot);

    // Marks a span of the commands recorded between them; `name` must outlive the timer. Spans
    // before the slot's first read, or past most_spans, aren't timed, nor is one never ended.
    void begin(VkCommandBuffer commands, std::uint32_t slot, std::string_view name) noexcept;
    void end(VkCommandBuffer commands, std::uint32_t slot) noexcept;

private:
    struct Slot {
        std::vector<std::string_view> names; // of the spans begun, each with two queries
        std::vector<GpuSpan> spans;          // read back
        bool ready = false;                  // its queries reset, so spans can begin
        bool open = false;
    };

    void release() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    double milliseconds_per_tick_ = 0.0;
    std::uint64_t tick_mask_ = 0;
    std::vector<Slot> slots_;
};

} // namespace hog::render
