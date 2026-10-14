#include <hog/core/log.hpp>
#include <hog/render/device.hpp>

#include <VkBootstrap.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace hog::render {

namespace {

const log::Category vulkan_log{"vulkan"};

template <typename T>
std::unexpected<Error> bootstrap_error(const vkb::Result<T>& result, std::string_view doing) {
    const ErrorCode code = result.vk_result() == VK_SUCCESS ||
                                   result.vk_result() == VK_ERROR_INCOMPATIBLE_DRIVER ||
                                   result.vk_result() == VK_ERROR_EXTENSION_NOT_PRESENT ||
                                   result.vk_result() == VK_ERROR_FEATURE_NOT_PRESENT
                               ? ErrorCode::Unsupported
                               : ErrorCode::Internal;
    return make_error(code, std::format("{}: {}", doing, result.error().message()));
}

bool validation_installed() {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return false;
    }
    return std::ranges::any_of(layers, [](const VkLayerProperties& layer) {
        return std::string_view{std::data(layer.layerName)} == "VK_LAYER_KHRONOS_validation";
    });
}

} // namespace

VkBool32 Device::report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                        VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* device = static_cast<Device*>(user);
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        if (device != nullptr) {
            device->validation_errors_.fetch_add(1, std::memory_order_relaxed);
        }
        log::error(vulkan_log, "{}", data->pMessage);
    } else {
        log::warn(vulkan_log, "{}", data->pMessage);
    }
    return VK_FALSE;
}

Result<std::unique_ptr<Device>> Device::create(const DeviceOptions& options) {
    if (const VkResult loaded = volkInitialize(); loaded != VK_SUCCESS) {
        return vulkan_error(loaded, "loading Vulkan");
    }
    // Not make_unique: the constructor is private.
    std::unique_ptr<Device> device{new Device};

    device->validating_ = options.validation && validation_installed();
    if (options.validation && !device->validating_) {
        log::warn(vulkan_log, "validation was asked for, but its layer isn't installed");
    }
    vkb::InstanceBuilder instance_builder{vkGetInstanceProcAddr};
    instance_builder.set_app_name(options.app_name)
        .require_api_version(1, 3, 0)
        .set_headless(!options.make_surface);
    if (device->validating_) {
        instance_builder.enable_validation_layers()
            .set_debug_callback(&Device::report)
            .set_debug_callback_user_data_pointer(device.get())
            .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
    }
    for (const std::string& extension : options.instance_extensions) {
        instance_builder.enable_extension(extension);
    }
    const auto instance = instance_builder.build();
    if (!instance) {
        return bootstrap_error(instance, "creating a Vulkan instance");
    }
    device->instance_ = instance->instance;
    device->messenger_ = instance->debug_messenger;
    volkLoadInstance(device->instance_);

    if (options.make_surface) {
        auto surface = options.make_surface(device->instance_);
        if (!surface) {
            return std::unexpected(std::move(surface).error());
        }
        device->surface_ = *surface;
    }

    // Slang reads vertex and instance ids through draw parameters.
    VkPhysicalDeviceVulkan11Features features_11{};
    features_11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceVulkan13Features features_13{};
    features_13.dynamicRendering = VK_TRUE;
    features_13.synchronization2 = VK_TRUE;
    vkb::PhysicalDeviceSelector selector{*instance};
    selector.set_minimum_version(1, 3)
        .set_required_features_11(features_11)
        .set_required_features_13(features_13)
        .require_present(device->surface_ != VK_NULL_HANDLE)
        .prefer_gpu_device_type(options.prefer_software ? vkb::PreferredDeviceType::cpu
                                                        : vkb::PreferredDeviceType::discrete);
    if (device->surface_ != VK_NULL_HANDLE) {
        selector.set_surface(device->surface_);
    }
    const auto physical = selector.select();
    if (!physical) {
        return bootstrap_error(physical, "choosing a Vulkan 1.3 device");
    }
    device->physical_ = physical->physical_device;
    device->name_ = physical->name;
    device->software_ = physical->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;

    const auto built = vkb::DeviceBuilder{*physical}.build();
    if (!built) {
        return bootstrap_error(built, "creating the Vulkan device");
    }
    device->device_ = built->device;
    volkLoadDevice(device->device_);
    const auto family = built->get_queue_index(vkb::QueueType::graphics);
    const auto queue = built->get_queue(vkb::QueueType::graphics);
    if (!family || !queue) {
        return make_error(ErrorCode::Unsupported, "the Vulkan device has no graphics queue");
    }
    device->queue_family_ = *family;
    device->queue_ = *queue;

    VmaVulkanFunctions functions{};
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.physicalDevice = device->physical_;
    allocator_info.device = device->device_;
    allocator_info.instance = device->instance_;
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
    if (const VkResult imported = vmaImportVulkanFunctionsFromVolk(&allocator_info, &functions);
        imported != VK_SUCCESS) {
        return vulkan_error(imported, "loading the allocator's Vulkan functions");
    }
    allocator_info.pVulkanFunctions = &functions;
    if (const VkResult made = vmaCreateAllocator(&allocator_info, &device->allocator_);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating the GPU memory allocator");
    }

    const VkCommandPoolCreateInfo pool_info{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                            .pNext = nullptr,
                                            .flags =
                                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                            .queueFamilyIndex = device->queue_family_};
    if (const VkResult made =
            vkCreateCommandPool(device->device_, &pool_info, nullptr, &device->pool_);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating a command pool");
    }
    const VkCommandBufferAllocateInfo commands_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = device->pool_,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1};
    if (const VkResult made =
            vkAllocateCommandBuffers(device->device_, &commands_info, &device->commands_);
        made != VK_SUCCESS) {
        return vulkan_error(made, "allocating a command buffer");
    }
    const VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    if (const VkResult made = vkCreateFence(device->device_, &fence_info, nullptr, &device->done_);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating a fence");
    }

    log::info(vulkan_log, "rendering on {}{}", device->name_,
              device->validating_ ? ", validated" : "");
    return device;
}

Device::~Device() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (done_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_, done_, nullptr);
        }
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, pool_, nullptr);
        }
        if (allocator_ != VK_NULL_HANDLE) {
            vmaDestroyAllocator(allocator_);
        }
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }
        if (messenger_ != VK_NULL_HANDLE) {
            vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        }
        vkDestroyInstance(instance_, nullptr);
    }
}

Result<> Device::submit_now(const std::function<void(VkCommandBuffer)>& record) {
    if (const VkResult reset = vkResetCommandBuffer(commands_, 0); reset != VK_SUCCESS) {
        return vulkan_error(reset, "resetting a command buffer");
    }
    const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .pNext = nullptr,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                         .pInheritanceInfo = nullptr};
    if (const VkResult begun = vkBeginCommandBuffer(commands_, &begin); begun != VK_SUCCESS) {
        return vulkan_error(begun, "beginning a command buffer");
    }
    record(commands_);
    if (const VkResult ended = vkEndCommandBuffer(commands_); ended != VK_SUCCESS) {
        return vulkan_error(ended, "recording commands");
    }
    const VkCommandBufferSubmitInfo commands_info{.sType =
                                                      VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                                  .pNext = nullptr,
                                                  .commandBuffer = commands_,
                                                  .deviceMask = 0};
    const VkSubmitInfo2 submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                               .pNext = nullptr,
                               .flags = 0,
                               .waitSemaphoreInfoCount = 0,
                               .pWaitSemaphoreInfos = nullptr,
                               .commandBufferInfoCount = 1,
                               .pCommandBufferInfos = &commands_info,
                               .signalSemaphoreInfoCount = 0,
                               .pSignalSemaphoreInfos = nullptr};
    if (const VkResult submitted = vkQueueSubmit2(queue_, 1, &submit, done_);
        submitted != VK_SUCCESS) {
        return vulkan_error(submitted, "submitting commands");
    }
    if (const VkResult waited =
            vkWaitForFences(device_, 1, &done_, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
        waited != VK_SUCCESS) {
        return vulkan_error(waited, "waiting for the GPU");
    }
    if (const VkResult reset = vkResetFences(device_, 1, &done_); reset != VK_SUCCESS) {
        return vulkan_error(reset, "resetting a fence");
    }
    return {};
}

} // namespace hog::render
