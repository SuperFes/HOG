#include "test_device.hpp"

#include <hog/render/presenter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <utility>

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
    const VkRenderingAttachmentInfo colour{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = frame.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {.float32 = {0.1F, 0.2F, 0.3F, 1.0F}}}};
    const VkRenderingInfo rendering{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                    .pNext = nullptr,
                                    .flags = 0,
                                    .renderArea = {.offset = {}, .extent = frame.extent},
                                    .layerCount = 1,
                                    .viewMask = 0,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &colour,
                                    .pDepthAttachment = nullptr,
                                    .pStencilAttachment = nullptr};
    vkCmdBeginRendering(frame.commands, &rendering);
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
