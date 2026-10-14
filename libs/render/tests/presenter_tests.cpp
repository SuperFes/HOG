#include "test_device.hpp"

#include <hog/render/memory.hpp>
#include <hog/render/presenter.hpp>
#include <hog/render/readback.hpp>
#include <hog/render/rendering.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

using hog::render::Frame;
using hog::render::needs_rebuild;
using hog::render::Presenter;

namespace {

// A device showing frames on a surface with no window, which software drivers offer.
std::unique_ptr<hog::render::Device> headless_device() {
    return hog::render::testing::test_device(
        {.instance_extensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                 VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME},
         .make_surface = [](VkInstance instance) -> hog::Result<VkSurfaceKHR> {
             const VkHeadlessSurfaceCreateInfoEXT info{
                 .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
                 .pNext = nullptr,
                 .flags = 0};
             VkSurfaceKHR surface = VK_NULL_HANDLE;
             if (const VkResult made =
                     vkCreateHeadlessSurfaceEXT(instance, &info, nullptr, &surface);
                 made != VK_SUCCESS) {
                 return hog::render::vulkan_error(made, "creating a headless surface");
             }
             return surface;
         }});
}

void clear(Frame& frame) {
    frame.target.use_as(frame.commands, hog::render::image_use::colour_target);
    hog::render::begin_rendering(frame.commands, frame.view, frame.extent,
                                 VkClearColorValue{.float32 = {0.2F, 0.4F, 1.0F, 1.0F}});
    vkCmdEndRendering(frame.commands);
}

// Draws and shows one frame, returning its size, which is zero when there was nothing to draw into.
VkExtent2D show(Presenter& presenter, VkExtent2D window) {
    auto begun = presenter.begin(window);
    REQUIRE(begun);
    std::optional<Frame> frame = std::move(begun).value();
    if (!frame.has_value()) {
        return {};
    }
    clear(frame.value());
    REQUIRE(presenter.end(frame.value()));
    return frame.value().extent;
}

// Clears a frame and copies it out before it's shown, returning it as RGB; empty when there was
// nothing to draw into.
std::vector<std::uint8_t> clear_and_read(const hog::render::Device& device, Presenter& presenter,
                                         VkExtent2D window) {
    auto begun = presenter.begin(window);
    REQUIRE(begun);
    std::optional<Frame> frame = std::move(begun).value();
    if (!frame.has_value()) {
        return {};
    }
    auto buffer = hog::render::make_readback(device, frame->extent);
    REQUIRE(buffer);
    clear(*frame);
    hog::render::record_readback(frame->commands, frame->target, frame->extent, *buffer);
    REQUIRE(presenter.end(*frame));
    REQUIRE(vkDeviceWaitIdle(device.handle()) == VK_SUCCESS);
    const auto pixels = hog::render::read_back(*buffer);
    REQUIRE(pixels);
    return hog::render::to_rgb(*pixels, frame->format).value_or(std::vector<std::uint8_t>{});
}

} // namespace

TEST_CASE("the swapchain is rebuilt when stale or resized", "[render]") {
    const VkExtent2D size{.width = 640, .height = 480};
    CHECK_FALSE(needs_rebuild(false, size, size));
    CHECK(needs_rebuild(true, size, size));
    CHECK(needs_rebuild(false, size, {.width = 640, .height = 481}));
    CHECK(needs_rebuild(false, size, {.width = 641, .height = 480}));
}

TEST_CASE("frames are shown, follow the window's size and pause while it's minimised",
          "[render][gpu]") {
    const auto device = headless_device();
    auto presenter = Presenter::create(*device, {.width = 64, .height = 48});
    REQUIRE(presenter);
    CHECK((*presenter)->swapchain().images().size() >= 2);

    for (int frame = 0; frame < 5; ++frame) {
        const VkExtent2D shown = show(**presenter, {.width = 64, .height = 48});
        CHECK(shown.width == 64);
        CHECK(shown.height == 48);
    }

    const VkExtent2D resized = show(**presenter, {.width = 100, .height = 30});
    CHECK(resized.width == 100);
    CHECK(resized.height == 30);
    CHECK((*presenter)->swapchain().extent().width == 100);

    CHECK(show(**presenter, {.width = 0, .height = 30}).width == 0);
    CHECK(show(**presenter, {.width = 100, .height = 0}).width == 0);

    CHECK(show(**presenter, {.width = 64, .height = 48}).width == 64);
    CHECK(device->validation_errors() == 0);
}

TEST_CASE("a frame can be read back before it's shown", "[render][gpu]") {
    const auto device = headless_device();
    auto presenter = Presenter::create(*device, {.width = 8, .height = 4}, {.srgb = false});
    REQUIRE(presenter);
    if (((*presenter)->swapchain().usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        SKIP("the surface's images can't be copied from");
    }
    const std::vector<std::uint8_t> rgb =
        clear_and_read(*device, **presenter, {.width = 8, .height = 4});
    REQUIRE(rgb.size() == std::size_t{3} * 8 * 4);
    CHECK(std::vector<std::uint8_t>(rgb.begin(), rgb.begin() + 3) ==
          std::vector<std::uint8_t>{51, 102, 255});
    CHECK(device->validation_errors() == 0);
}
