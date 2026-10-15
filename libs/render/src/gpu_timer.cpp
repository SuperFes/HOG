#include <hog/render/device.hpp>
#include <hog/render/gpu_timer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace hog::render {

namespace {

constexpr std::uint32_t queries_per_slot = 2 * GpuTimer::most_spans;

// A query's ticks, and whether it was written.
struct Stamp {
    std::uint64_t ticks = 0;
    std::uint64_t available = 0;
};

} // namespace

GpuTimer::GpuTimer(GpuTimer&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
      milliseconds_per_tick_(other.milliseconds_per_tick_), tick_mask_(other.tick_mask_),
      slots_(std::move(other.slots_)) {}

GpuTimer& GpuTimer::operator=(GpuTimer&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
        milliseconds_per_tick_ = other.milliseconds_per_tick_;
        tick_mask_ = other.tick_mask_;
        slots_ = std::move(other.slots_);
    }
    return *this;
}

GpuTimer::~GpuTimer() {
    release();
}

void GpuTimer::release() noexcept {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
}

Result<GpuTimer> GpuTimer::create(const Device& device, std::uint32_t frames) {
    GpuTimer timer;
    timer.slots_.resize(frames);
    // So spans begin without allocating.
    for (Slot& slot : timer.slots_) {
        slot.names.reserve(most_spans);
        slot.spans.reserve(most_spans);
    }
    const std::uint32_t bits = device.timestamp_bits();
    if (bits == 0 || frames == 0) {
        return timer;
    }
    const VkQueryPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .pNext = nullptr,
                                     .flags = 0,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = frames * queries_per_slot,
                                     .pipelineStatistics = 0};
    if (const VkResult made = vkCreateQueryPool(device.handle(), &info, nullptr, &timer.pool_);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating the GPU timer's queries");
    }
    timer.device_ = device.handle();
    timer.milliseconds_per_tick_ = device.timestamp_period() * 1e-6;
    timer.tick_mask_ = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    return timer;
}

std::span<const GpuSpan> GpuTimer::read(VkCommandBuffer commands, std::uint32_t slot) {
    if (!timing()) {
        return {};
    }
    Slot& at = slots_.at(slot);
    at.spans.clear();
    const std::uint32_t first = slot * queries_per_slot;
    if (!at.names.empty()) {
        std::array<Stamp, queries_per_slot> stamps{};
        const auto count = static_cast<std::uint32_t>(2 * at.names.size());
        // Without waiting: a span whose frame was never submitted is left out.
        vkGetQueryPoolResults(device_, pool_, first, count, count * sizeof(Stamp), stamps.data(),
                              sizeof(Stamp),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        for (std::size_t span = 0; span < at.names.size(); ++span) {
            const Stamp& began = stamps.at(2 * span);
            const Stamp& ended = stamps.at((2 * span) + 1);
            if (began.available == 0 || ended.available == 0) {
                continue;
            }
            // Masked, so a counter wrapping round between them still gives the span.
            const std::uint64_t ticks = (ended.ticks - began.ticks) & tick_mask_;
            at.spans.push_back(
                {.name = at.names[span],
                 .milliseconds = static_cast<double>(ticks) * milliseconds_per_tick_});
        }
    }
    at.names.clear();
    at.ready = true;
    at.open = false;
    vkCmdResetQueryPool(commands, pool_, first, queries_per_slot);
    return at.spans;
}

void GpuTimer::begin(VkCommandBuffer commands, std::uint32_t slot, std::string_view name) noexcept {
    if (!timing()) {
        return;
    }
    Slot& at = slots_[slot];
    if (!at.ready || at.open || at.names.size() == most_spans) {
        return;
    }
    const std::uint32_t query =
        (slot * queries_per_slot) + static_cast<std::uint32_t>(2 * at.names.size());
    vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, query);
    at.names.push_back(name);
    at.open = true;
}

void GpuTimer::end(VkCommandBuffer commands, std::uint32_t slot) noexcept {
    if (!timing()) {
        return;
    }
    Slot& at = slots_[slot];
    if (!at.open) {
        return;
    }
    const std::uint32_t query =
        (slot * queries_per_slot) + static_cast<std::uint32_t>((2 * at.names.size()) - 1);
    vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, query);
    at.open = false;
}

} // namespace hog::render
