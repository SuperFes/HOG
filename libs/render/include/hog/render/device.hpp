#pragma once

#include <hog/core/result.hpp>
#include <hog/render/vulkan.hpp>

#include <vk_mem_alloc.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hog::render {

struct DeviceOptions {
    std::string app_name = "HOG";
    // The Khronos validation layer, where it's installed; what it reports is logged and counted.
    bool validation = false;
    // A software device such as lavapipe over a GPU, so images come out the same on any machine.
    bool prefer_software = false;
    // Instance extensions the window system needs to make its surface.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::vector<std::string> instance_extensions = {};
    // Makes the window's surface once the instance exists. Without one, the device can only
    // render off-screen.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::function<Result<VkSurfaceKHR>(VkInstance)> make_surface = {};
};

// A Vulkan 1.3 instance, device and graphics queue, with an allocator for GPU memory. Vulkan's
// functions are loaded process-wide, so there is one device at a time.
class Device {
public:
    [[nodiscard]] static Result<std::unique_ptr<Device>> create(const DeviceOptions& options);

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;
    ~Device();

    [[nodiscard]] VkInstance instance() const noexcept { return instance_; }
    [[nodiscard]] VkPhysicalDevice physical() const noexcept { return physical_; }
    [[nodiscard]] VkDevice handle() const noexcept { return device_; }
    [[nodiscard]] VmaAllocator allocator() const noexcept { return allocator_; }
    [[nodiscard]] VkQueue queue() const noexcept { return queue_; }
    [[nodiscard]] std::uint32_t queue_family() const noexcept { return queue_family_; }
    // VK_NULL_HANDLE when rendering off-screen only.
    [[nodiscard]] VkSurfaceKHR surface() const noexcept { return surface_; }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool software() const noexcept { return software_; }
    [[nodiscard]] bool validating() const noexcept { return validating_; }
    // Errors the validation layer has reported.
    [[nodiscard]] std::uint64_t validation_errors() const noexcept {
        return validation_errors_.load(std::memory_order_relaxed);
    }

    // Records commands with `record`, submits them and waits for the GPU to finish them.
    Result<> submit_now(const std::function<void(VkCommandBuffer)>& record);

private:
    Device() = default;

    static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT types,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                 void* user);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    VkFence done_ = VK_NULL_HANDLE;
    std::string name_;
    bool software_ = false;
    bool validating_ = false;
    std::atomic<std::uint64_t> validation_errors_{0};
};

} // namespace hog::render
