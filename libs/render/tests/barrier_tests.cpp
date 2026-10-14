#include <hog/render/barrier.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace use = hog::render::image_use;
using hog::render::needs_barrier;
using hog::render::TrackedImage;

TEST_CASE("barriers come between writes and layout changes, not between reads", "[render]") {
    CHECK(needs_barrier(use::colour_target, use::colour_target));
    CHECK(needs_barrier(use::copy_target, use::sampled));
    CHECK(needs_barrier(use::sampled, use::copy_source));
    CHECK_FALSE(needs_barrier(use::sampled, use::sampled));
    CHECK_FALSE(needs_barrier(use::present, use::present));
}

TEST_CASE("a tracked image waits on its last use", "[render]") {
    // A made-up handle, never used.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* image = reinterpret_cast<VkImage>(std::uintptr_t{0x1000});
    TrackedImage tracked{image, VK_IMAGE_ASPECT_COLOR_BIT};
    CHECK(tracked.use() == use::discard);

    const auto into_target = tracked.barrier_to(use::colour_target);
    REQUIRE(into_target.has_value());
    const VkImageMemoryBarrier2 first = into_target.value_or(VkImageMemoryBarrier2{});
    CHECK(first.image == image);
    CHECK(first.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(first.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(tracked.use() == use::colour_target);

    const auto into_sampled = tracked.barrier_to(use::sampled);
    REQUIRE(into_sampled.has_value());
    const VkImageMemoryBarrier2 second = into_sampled.value_or(VkImageMemoryBarrier2{});
    CHECK(second.srcStageMask == use::colour_target.stage);
    CHECK(second.srcAccessMask == use::colour_target.access);
    CHECK(second.dstAccessMask == use::sampled.access);
    CHECK(second.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);

    CHECK_FALSE(tracked.barrier_to(use::sampled));
    CHECK(tracked.use() == use::sampled);
}
