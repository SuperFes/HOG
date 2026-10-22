#include <hog/render/device.hpp>
#include <hog/render/presenter.hpp>

#include <limits>
#include <utility>

namespace hog::render {

namespace {

constexpr std::uint64_t forever = std::numeric_limits<std::uint64_t>::max();

// The first use of an acquired image must wait for the display to let go of it, so the stages that
// can start with it wait on the acquire.
constexpr VkPipelineStageFlags2 first_use_stages =
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
constexpr ImageUse acquired{
    .stage = first_use_stages, .access = VK_ACCESS_2_NONE, .layout = VK_IMAGE_LAYOUT_UNDEFINED};

bool has_area(VkExtent2D extent) noexcept {
    return extent.width != 0 && extent.height != 0;
}

Result<VkSemaphore> make_semaphore(VkDevice device) {
    const VkSemaphoreCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (const VkResult made = vkCreateSemaphore(device, &info, nullptr, &semaphore);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating a semaphore");
    }
    return semaphore;
}

} // namespace

bool needs_rebuild(bool stale, VkExtent2D built_for, VkExtent2D window) noexcept {
    return stale || built_for.width != window.width || built_for.height != window.height;
}

Result<std::unique_ptr<Presenter>> Presenter::create(Device& device, VkExtent2D window,
                                                     const SwapchainOptions& options) {
    auto swapchain = make_swapchain(device, window, options);
    if (!swapchain) {
        return std::unexpected(std::move(swapchain).error());
    }
    // Not make_unique: the constructor is private.
    std::unique_ptr<Presenter> presenter{new Presenter{device}};
    presenter->swapchain_ = std::move(swapchain).value();
    presenter->built_for_ = window;
    // An empty swapchain is retried on the next frame.
    presenter->stale_ = presenter->swapchain_.handle() == VK_NULL_HANDLE;

    VkDevice handle = device.handle();
    for (Slot& slot : presenter->slots_) {
        const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                .pNext = nullptr,
                                                .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                                .queueFamilyIndex = device.queue_family()};
        if (const VkResult made = vkCreateCommandPool(handle, &pool_info, nullptr, &slot.pool);
            made != VK_SUCCESS) {
            return vulkan_error(made, "creating a frame's command pool");
        }
        const VkCommandBufferAllocateInfo commands_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = slot.pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1};
        if (const VkResult made = vkAllocateCommandBuffers(handle, &commands_info, &slot.commands);
            made != VK_SUCCESS) {
            return vulkan_error(made, "allocating a frame's command buffer");
        }
        auto acquired_semaphore = make_semaphore(handle);
        if (!acquired_semaphore) {
            return std::unexpected(std::move(acquired_semaphore).error());
        }
        slot.acquired = *acquired_semaphore;
        // Signalled, so the first wait for it returns at once.
        const VkFenceCreateInfo fence_info{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                           .pNext = nullptr,
                                           .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        if (const VkResult made = vkCreateFence(handle, &fence_info, nullptr, &slot.done);
            made != VK_SUCCESS) {
            return vulkan_error(made, "creating a frame's fence");
        }
    }
    for (std::size_t image = 0; image < presenter->swapchain_.images().size(); ++image) {
        auto rendered = make_semaphore(handle);
        if (!rendered) {
            return std::unexpected(std::move(rendered).error());
        }
        presenter->rendered_.push_back(*rendered);
    }
    return presenter;
}

Presenter::~Presenter() {
    VkDevice handle = device_->handle();
    vkDeviceWaitIdle(handle);
    for (VkSemaphore semaphore : rendered_) {
        vkDestroySemaphore(handle, semaphore, nullptr);
    }
    for (const Slot& slot : slots_) {
        if (slot.done != VK_NULL_HANDLE) {
            vkDestroyFence(handle, slot.done, nullptr);
        }
        if (slot.acquired != VK_NULL_HANDLE) {
            vkDestroySemaphore(handle, slot.acquired, nullptr);
        }
        if (slot.pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(handle, slot.pool, nullptr);
        }
    }
}

Result<> Presenter::rebuild(VkExtent2D window) {
    VkDevice handle = device_->handle();
    if (const VkResult waited = vkDeviceWaitIdle(handle); waited != VK_SUCCESS) {
        return vulkan_error(waited, "waiting for the GPU before rebuilding the swapchain");
    }
    if (auto built = swapchain_.rebuild(window); !built) {
        return built;
    }
    built_for_ = window;
    stale_ = swapchain_.handle() == VK_NULL_HANDLE;
    while (rendered_.size() < swapchain_.images().size()) {
        auto rendered = make_semaphore(handle);
        if (!rendered) {
            return std::unexpected(std::move(rendered).error());
        }
        rendered_.push_back(*rendered);
    }
    return {};
}

Result<std::optional<Frame>> Presenter::begin(VkExtent2D window) {
    if (!has_area(window)) {
        return std::nullopt;
    }
    if (needs_rebuild(stale_, built_for_, window)) {
        if (auto rebuilt = rebuild(window); !rebuilt) {
            return std::unexpected(std::move(rebuilt).error());
        }
    }
    if (swapchain_.handle() == VK_NULL_HANDLE) {
        return std::nullopt;
    }

    VkDevice handle = device_->handle();
    const Slot& slot = slots_.at(slot_);
    if (const VkResult waited = vkWaitForFences(handle, 1, &slot.done, VK_TRUE, forever);
        waited != VK_SUCCESS) {
        return vulkan_error(waited, "waiting for a frame in flight");
    }
    std::uint32_t image = 0;
    const VkResult acquire = vkAcquireNextImageKHR(handle, swapchain_.handle(), forever,
                                                   slot.acquired, VK_NULL_HANDLE, &image);
    const auto stale = swapchain_stale(acquire, "taking a swapchain image");
    if (!stale) {
        return std::unexpected(stale.error());
    }
    stale_ = *stale;
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        // Nothing was acquired, so the fence stays signalled for the next try.
        return std::nullopt;
    }

    if (const VkResult reset = vkResetFences(handle, 1, &slot.done); reset != VK_SUCCESS) {
        return vulkan_error(reset, "resetting a frame's fence");
    }
    if (const VkResult reset = vkResetCommandPool(handle, slot.pool, 0); reset != VK_SUCCESS) {
        return vulkan_error(reset, "resetting a frame's commands");
    }
    const VkCommandBufferBeginInfo begin_info{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                              .pNext = nullptr,
                                              .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                              .pInheritanceInfo = nullptr};
    if (const VkResult begun = vkBeginCommandBuffer(slot.commands, &begin_info);
        begun != VK_SUCCESS) {
        return vulkan_error(begun, "beginning a frame's commands");
    }
    return Frame{.commands = slot.commands,
                 .target =
                     TrackedImage{swapchain_.images()[image], VK_IMAGE_ASPECT_COLOR_BIT, acquired},
                 .view = swapchain_.views()[image],
                 .format = swapchain_.format(),
                 .extent = swapchain_.extent(),
                 .slot = slot_,
                 .image = image,
                 .copyable = (swapchain_.usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0};
}

Result<> Presenter::end(Frame& frame) {
    const Slot& slot = slots_.at(frame.slot);
    frame.target.use_as(frame.commands, image_use::present);
    if (const VkResult ended = vkEndCommandBuffer(frame.commands); ended != VK_SUCCESS) {
        return vulkan_error(ended, "recording a frame");
    }

    VkSemaphore rendered = rendered_.at(frame.image);
    const VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                     .pNext = nullptr,
                                     .semaphore = slot.acquired,
                                     .value = 0,
                                     .stageMask = first_use_stages,
                                     .deviceIndex = 0};
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .pNext = nullptr,
                                       .semaphore = rendered,
                                       .value = 0,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       .deviceIndex = 0};
    const VkCommandBufferSubmitInfo commands{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                             .pNext = nullptr,
                                             .commandBuffer = frame.commands,
                                             .deviceMask = 0};
    const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                               .pNext = nullptr,
                               .flags = 0,
                               .waitSemaphoreInfoCount = 1,
                               .pWaitSemaphoreInfos = &wait,
                               .commandBufferInfoCount = 1,
                               .pCommandBufferInfos = &commands,
                               .signalSemaphoreInfoCount = 1,
                               .pSignalSemaphoreInfos = &signal};
    if (const VkResult submitted = vkQueueSubmit2(device_->queue(), 1, &submit, slot.done);
        submitted != VK_SUCCESS) {
        return vulkan_error(submitted, "submitting a frame");
    }
    slot_ = (slot_ + 1) % frames_in_flight;

    VkSwapchainKHR swapchain = swapchain_.handle();
    const VkPresentInfoKHR present{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                   .pNext = nullptr,
                                   .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &rendered,
                                   .swapchainCount = 1,
                                   .pSwapchains = &swapchain,
                                   .pImageIndices = &frame.image,
                                   .pResults = nullptr};
    const auto stale =
        swapchain_stale(vkQueuePresentKHR(device_->queue(), &present), "showing a frame");
    if (!stale) {
        return std::unexpected(stale.error());
    }
    stale_ = stale_ || *stale;
    return {};
}

} // namespace hog::render
