#include <hog/render/device.hpp>
#include <hog/render/gpu_timer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#ifdef HOG_PROFILE
#include <client/TracyProfiler.hpp>
#include <tracy/TracyC.h>

#include <source_location>
#include <string_view>
#endif

namespace hog::render {

namespace {

constexpr std::uint32_t queries_per_slot = 2 * GpuTimer::most_spans;

// A query's ticks, and whether it was written.
struct Stamp {
    std::uint64_t ticks = 0;
    std::uint64_t available = 0;
};

#ifdef HOG_PROFILE
// Names a GPU context for the profiler, with the GPU's clock beside the CPU's: a timestamp written
// and waited for, so its time is a moment ago on the CPU too. Clocks drift apart in time.
Result<std::uint8_t> profile_context(Device& device, VkQueryPool pool) {
    const auto stamped = device.submit_now([&](VkCommandBuffer commands) {
        vkCmdResetQueryPool(commands, pool, 0, 1);
        vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, 0);
    });
    if (!stamped) {
        return std::unexpected(stamped.error());
    }
    std::uint64_t ticks = 0;
    if (const VkResult got =
            vkGetQueryPoolResults(device.handle(), pool, 0, 1, sizeof(ticks), &ticks, sizeof(ticks),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        got != VK_SUCCESS) {
        return vulkan_error(got, "reading the GPU's clock");
    }
    const auto context = static_cast<std::uint8_t>(tracy::GetGpuCtxCounter().fetch_add(1));
    ___tracy_emit_gpu_new_context_serial(
        {.gpuTime = static_cast<std::int64_t>(ticks),
         .period = static_cast<float>(device.timestamp_period()),
         .context = context,
         .flags = 0,
         .type = static_cast<std::uint8_t>(tracy::GpuContextType::Vulkan)});
    constexpr std::string_view name = "frame";
    ___tracy_emit_gpu_context_name_serial(
        {.context = context, .name = name.data(), .len = static_cast<std::uint16_t>(name.size())});
    return context;
}
#endif

} // namespace

GpuTimer::GpuTimer(GpuTimer&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)),
      milliseconds_per_tick_(other.milliseconds_per_tick_), tick_mask_(other.tick_mask_),
      context_(other.context_), slots_(std::move(other.slots_)) {}

GpuTimer& GpuTimer::operator=(GpuTimer&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
        milliseconds_per_tick_ = other.milliseconds_per_tick_;
        tick_mask_ = other.tick_mask_;
        context_ = other.context_;
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

Result<GpuTimer> GpuTimer::create(Device& device, std::uint32_t frames) {
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
#ifdef HOG_PROFILE
    auto context = profile_context(device, timer.pool_);
    if (!context) {
        return std::unexpected(std::move(context).error());
    }
    timer.context_ = *context;
#endif
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
#ifdef HOG_PROFILE
            if (((at.profiled >> span) & 1U) != 0) {
                const auto query = static_cast<std::uint16_t>(first + (2 * span));
                ___tracy_emit_gpu_time_serial({.gpuTime = static_cast<std::int64_t>(began.ticks),
                                               .queryId = query,
                                               .context = context_});
                ___tracy_emit_gpu_time_serial({.gpuTime = static_cast<std::int64_t>(ended.ticks),
                                               .queryId = static_cast<std::uint16_t>(query + 1),
                                               .context = context_});
            }
#endif
            // Masked, so a counter wrapping round between them still gives the span.
            const std::uint64_t ticks = (ended.ticks - began.ticks) & tick_mask_;
            at.spans.push_back(
                {.name = at.names[span],
                 .milliseconds = static_cast<double>(ticks) * milliseconds_per_tick_});
        }
    }
#ifdef HOG_PROFILE
    // The profiler would nest every later span in one left open.
    if (at.open && ((at.profiled >> (at.names.size() - 1)) & 1U) != 0) {
        ___tracy_emit_gpu_zone_end_serial(
            {.queryId = static_cast<std::uint16_t>(first + (2 * at.names.size()) - 1),
             .context = context_});
    }
#endif
    at.names.clear();
    at.profiled = 0;
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
#ifdef HOG_PROFILE
    // Only while connected, and remembered, so the profiler never hears of a span's end or times
    // without its beginning.
    if (___tracy_connected() != 0) {
        constexpr std::source_location here = std::source_location::current();
        const std::string_view file = here.file_name();
        const std::string_view function = here.function_name();
        const std::uint64_t location =
            ___tracy_alloc_srcloc_name(here.line(), file.data(), file.size(), function.data(),
                                       function.size(), name.data(), name.size(), 0);
        ___tracy_emit_gpu_zone_begin_alloc_serial({.srcloc = location,
                                                   .queryId = static_cast<std::uint16_t>(query),
                                                   .context = context_});
        at.profiled |= std::uint64_t{1} << at.names.size();
    }
#endif
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
#ifdef HOG_PROFILE
    if (((at.profiled >> (at.names.size() - 1)) & 1U) != 0) {
        ___tracy_emit_gpu_zone_end_serial(
            {.queryId = static_cast<std::uint16_t>(query), .context = context_});
    }
#endif
    at.open = false;
}

} // namespace hog::render
