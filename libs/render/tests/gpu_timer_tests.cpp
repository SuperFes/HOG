#include "test_device.hpp"

#include <hog/render/device.hpp>
#include <hog/render/gpu_timer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using hog::render::GpuSpan;
using hog::render::GpuTimer;
using hog::render::testing::test_device;

namespace {

// A timer on the device, or a skipped test where its queue can't time.
GpuTimer timer_on(hog::render::Device& device) {
    auto timer = GpuTimer::create(device, 2);
    REQUIRE(timer);
    if (!timer->timing()) {
        SKIP("the Vulkan device's queue can't time");
    }
    return std::move(timer).value();
}

// What a slot's read gives, as a copy that outlasts the next read.
std::vector<GpuSpan> read(hog::render::Device& device, GpuTimer& timer, std::uint32_t slot) {
    std::vector<GpuSpan> spans;
    const auto submitted = device.submit_now([&](VkCommandBuffer commands) {
        const std::span<const GpuSpan> read = timer.read(commands, slot);
        spans.assign(read.begin(), read.end());
    });
    REQUIRE(submitted);
    return spans;
}

} // namespace

TEST_CASE("a GPU timer that can't time reads nothing", "[render][gpu_timer]") {
    GpuTimer timer;
    CHECK_FALSE(timer.timing());
    CHECK(timer.read(VK_NULL_HANDLE, 0).empty());
    timer.begin(VK_NULL_HANDLE, 0, "nothing");
    timer.end(VK_NULL_HANDLE, 0);
}

TEST_CASE("a GPU timer reads back a frame's spans, named and in order",
          "[render][gpu][gpu_timer]") {
    const auto device = test_device();
    GpuTimer timer = timer_on(*device);
    CHECK(read(*device, timer, 0).empty());
    const auto timed = device->submit_now([&](VkCommandBuffer commands) {
        timer.begin(commands, 0, "first");
        timer.end(commands, 0);
        timer.begin(commands, 0, "second");
        timer.end(commands, 0);
    });
    REQUIRE(timed);
    // Readying slot 1 too, which timed nothing.
    CHECK(read(*device, timer, 1).empty());
    const std::vector<GpuSpan> spans = read(*device, timer, 0);
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].name == "first");
    CHECK(spans[1].name == "second");
    for (const GpuSpan& span : spans) {
        CHECK(span.milliseconds >= 0.0);
        CHECK(span.milliseconds < 1'000.0);
    }
    // Read, so they're gone.
    CHECK(read(*device, timer, 0).empty());
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a GPU timer leaves out spans it can't time", "[render][gpu][gpu_timer]") {
    const auto device = test_device();
    GpuTimer timer = timer_on(*device);
    const auto timed = device->submit_now([&](VkCommandBuffer commands) {
        // Before the slot was first read, so its queries weren't ready.
        timer.begin(commands, 0, "early");
        timer.end(commands, 0);
    });
    REQUIRE(timed);
    CHECK(read(*device, timer, 0).empty());
    const auto more = device->submit_now([&](VkCommandBuffer commands) {
        for (std::uint32_t span = 0; span + 1 < GpuTimer::most_spans; ++span) {
            timer.begin(commands, 0, "many");
            timer.end(commands, 0);
        }
        timer.begin(commands, 0, "never ended");
    });
    REQUIRE(more);
    const std::vector<GpuSpan> spans = read(*device, timer, 0);
    CHECK(spans.size() == std::size_t{GpuTimer::most_spans} - 1);
    for (const GpuSpan& span : spans) {
        CHECK(span.name == "many");
    }
    const auto too_many = device->submit_now([&](VkCommandBuffer commands) {
        for (std::uint32_t span = 0; span < GpuTimer::most_spans + 4; ++span) {
            timer.begin(commands, 0, "many");
            timer.end(commands, 0);
        }
    });
    REQUIRE(too_many);
    CHECK(read(*device, timer, 0).size() == std::size_t{GpuTimer::most_spans});
    CHECK(device->validation_errors() == 0);
}
