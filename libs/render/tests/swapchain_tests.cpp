#include <hog/render/swapchain.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>

using hog::render::choose_extent;
using hog::render::choose_image_count;
using hog::render::choose_present_mode;
using hog::render::choose_surface_format;

namespace {

VkSurfaceCapabilitiesKHR capabilities(VkExtent2D current, std::uint32_t min_images,
                                      std::uint32_t max_images) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization): only extents and counts matter
    VkSurfaceCapabilitiesKHR result{};
    result.currentExtent = current;
    result.minImageExtent = {.width = 16, .height = 16};
    result.maxImageExtent = {.width = 4096, .height = 2048};
    result.minImageCount = min_images;
    result.maxImageCount = max_images;
    return result;
}

constexpr std::uint32_t undefined = std::numeric_limits<std::uint32_t>::max();

} // namespace

TEST_CASE("surface formats prefer 8-bit sRGB or plain colour as asked", "[render]") {
    constexpr auto space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    const std::array offered{
        VkSurfaceFormatKHR{.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32, .colorSpace = space},
        VkSurfaceFormatKHR{.format = VK_FORMAT_R8G8B8A8_UNORM, .colorSpace = space},
        VkSurfaceFormatKHR{.format = VK_FORMAT_R8G8B8A8_SRGB, .colorSpace = space},
        VkSurfaceFormatKHR{.format = VK_FORMAT_B8G8R8A8_UNORM, .colorSpace = space}};
    CHECK(choose_surface_format(offered, true).format == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(choose_surface_format(offered, false).format == VK_FORMAT_B8G8R8A8_UNORM);

    const std::array odd{
        VkSurfaceFormatKHR{.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32, .colorSpace = space}};
    CHECK(choose_surface_format(odd, true).format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    CHECK(choose_surface_format({}, true).format == VK_FORMAT_UNDEFINED);
}

TEST_CASE("present modes wait for the display unless told not to", "[render]") {
    const std::array all{VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR,
                         VK_PRESENT_MODE_MAILBOX_KHR};
    CHECK(choose_present_mode(all, true) == VK_PRESENT_MODE_FIFO_KHR);
    CHECK(choose_present_mode(all, false) == VK_PRESENT_MODE_MAILBOX_KHR);
    const std::array tearing{VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    CHECK(choose_present_mode(tearing, false) == VK_PRESENT_MODE_IMMEDIATE_KHR);
    const std::array fifo{VK_PRESENT_MODE_FIFO_KHR};
    CHECK(choose_present_mode(fifo, false) == VK_PRESENT_MODE_FIFO_KHR);
}

TEST_CASE("the swapchain is the surface's size, or the window's within limits", "[render]") {
    const VkExtent2D window{.width = 800, .height = 600};
    const auto fixed = choose_extent(capabilities({.width = 640, .height = 480}, 2, 3), window);
    CHECK(fixed.width == 640);
    CHECK(fixed.height == 480);

    const auto free = capabilities({.width = undefined, .height = undefined}, 2, 3);
    const auto fitted = choose_extent(free, window);
    CHECK(fitted.width == 800);
    CHECK(fitted.height == 600);
    const auto clamped = choose_extent(free, {.width = 8, .height = 9000});
    CHECK(clamped.width == 16);
    CHECK(clamped.height == 2048);

    // A minimised window on some systems.
    const auto none = choose_extent(capabilities({.width = 0, .height = 0}, 2, 3), window);
    CHECK(none.width == 0);
}

TEST_CASE("the swapchain asks for one image more than the least", "[render]") {
    CHECK(choose_image_count(capabilities({}, 2, 8)) == 3);
    CHECK(choose_image_count(capabilities({}, 3, 3)) == 3);
    CHECK(choose_image_count(capabilities({}, 2, 0)) == 3);
}

TEST_CASE("stale swapchains are told apart from failures", "[render]") {
    CHECK(hog::render::swapchain_stale(VK_SUCCESS, "showing") == false);
    CHECK(hog::render::swapchain_stale(VK_SUBOPTIMAL_KHR, "showing") == true);
    CHECK(hog::render::swapchain_stale(VK_ERROR_OUT_OF_DATE_KHR, "showing") == true);
    const auto lost = hog::render::swapchain_stale(VK_ERROR_SURFACE_LOST_KHR, "showing");
    REQUIRE_FALSE(lost);
    CHECK(lost.error().message == "showing: VK_ERROR_SURFACE_LOST_KHR");
}
