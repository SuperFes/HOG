#include <hog/render/device.hpp>
#include <hog/render/swapchain.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace hog::render {

namespace {

constexpr std::uint32_t undefined_extent = std::numeric_limits<std::uint32_t>::max();

bool has_area(VkExtent2D extent) noexcept {
    return extent.width != 0 && extent.height != 0;
}

} // namespace

VkSurfaceFormatKHR choose_surface_format(std::span<const VkSurfaceFormatKHR> available,
                                         bool srgb) noexcept {
    const std::array preferred =
        srgb ? std::array{VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}
             : std::array{VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    for (const VkFormat format : preferred) {
        const auto found = std::ranges::find_if(available, [&](const VkSurfaceFormatKHR& offered) {
            return offered.format == format &&
                   offered.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (found != available.end()) {
            return *found;
        }
    }
    return available.empty() ? VkSurfaceFormatKHR{} : available.front();
}

VkPresentModeKHR choose_present_mode(std::span<const VkPresentModeKHR> available,
                                     bool vsync) noexcept {
    if (!vsync) {
        for (const VkPresentModeKHR mode :
             {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::ranges::contains(available, mode)) {
                return mode;
            }
        }
    }
    // Every surface has it.
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D window) noexcept {
    if (capabilities.currentExtent.width != undefined_extent) {
        return capabilities.currentExtent;
    }
    return {.width = std::clamp(window.width, capabilities.minImageExtent.width,
                                capabilities.maxImageExtent.width),
            .height = std::clamp(window.height, capabilities.minImageExtent.height,
                                 capabilities.maxImageExtent.height)};
}

std::uint32_t choose_image_count(const VkSurfaceCapabilitiesKHR& capabilities) noexcept {
    const std::uint32_t wanted = capabilities.minImageCount + 1;
    // No upper limit is zero.
    return capabilities.maxImageCount == 0 ? wanted : std::min(wanted, capabilities.maxImageCount);
}

Result<bool> swapchain_stale(VkResult result, std::string_view doing) {
    switch (result) {
    case VK_SUCCESS:
        return false;
    case VK_SUBOPTIMAL_KHR:
    case VK_ERROR_OUT_OF_DATE_KHR:
        return true;
    default:
        return vulkan_error(result, doing);
    }
}

Swapchain::Swapchain(Swapchain&& other) noexcept
    : physical_(std::exchange(other.physical_, VK_NULL_HANDLE)),
      device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      surface_(std::exchange(other.surface_, VK_NULL_HANDLE)), options_(other.options_),
      swapchain_(std::exchange(other.swapchain_, VK_NULL_HANDLE)),
      format_(std::exchange(other.format_, VK_FORMAT_UNDEFINED)),
      extent_(std::exchange(other.extent_, {})), images_(std::exchange(other.images_, {})),
      views_(std::exchange(other.views_, {})) {}

Swapchain& Swapchain::operator=(Swapchain&& other) noexcept {
    if (this != &other) {
        release();
        physical_ = std::exchange(other.physical_, VK_NULL_HANDLE);
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        surface_ = std::exchange(other.surface_, VK_NULL_HANDLE);
        options_ = other.options_;
        swapchain_ = std::exchange(other.swapchain_, VK_NULL_HANDLE);
        format_ = std::exchange(other.format_, VK_FORMAT_UNDEFINED);
        extent_ = std::exchange(other.extent_, {});
        images_ = std::exchange(other.images_, {});
        views_ = std::exchange(other.views_, {});
    }
    return *this;
}

Swapchain::~Swapchain() {
    release();
}

void Swapchain::release_views() noexcept {
    for (VkImageView view : views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    views_.clear();
    images_.clear();
}

void Swapchain::release() noexcept {
    release_views();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

Result<> Swapchain::rebuild(VkExtent2D window) {
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization): Vulkan fills it in
    VkSurfaceCapabilitiesKHR capabilities{};
    if (const VkResult got =
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &capabilities);
        got != VK_SUCCESS) {
        return vulkan_error(got, "reading the surface's capabilities");
    }
    const VkExtent2D extent = choose_extent(capabilities, window);
    if (!has_area(extent)) {
        release();
        extent_ = {};
        return {};
    }

    std::uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (const VkResult got =
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data());
        got != VK_SUCCESS) {
        return vulkan_error(got, "reading the surface's formats");
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    if (const VkResult got =
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &count, modes.data());
        got != VK_SUCCESS) {
        return vulkan_error(got, "reading the surface's present modes");
    }
    const VkSurfaceFormatKHR format = choose_surface_format(formats, options_.srgb);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (const VkCompositeAlphaFlagBitsKHR offered :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if ((capabilities.supportedCompositeAlpha &
             static_cast<VkCompositeAlphaFlagsKHR>(offered)) != 0) {
            alpha = offered;
            break;
        }
    }
    const VkSwapchainCreateInfoKHR info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = surface_,
        .minImageCount = choose_image_count(capabilities),
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .preTransform = capabilities.currentTransform,
        .compositeAlpha = alpha,
        .presentMode = choose_present_mode(modes, options_.vsync),
        .clipped = VK_TRUE,
        .oldSwapchain = swapchain_};
    VkSwapchainKHR made = VK_NULL_HANDLE;
    const VkResult created = vkCreateSwapchainKHR(device_, &info, nullptr, &made);
    release();
    if (created != VK_SUCCESS) {
        return vulkan_error(created, "creating the swapchain");
    }
    swapchain_ = made;
    format_ = format.format;
    extent_ = extent;

    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    images_.resize(count);
    if (const VkResult got = vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());
        got != VK_SUCCESS) {
        return vulkan_error(got, "reading the swapchain's images");
    }
    for (VkImage image : images_) {
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format_,
            .components = {},
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = 0,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1}};
        VkImageView view = VK_NULL_HANDLE;
        if (const VkResult made_view = vkCreateImageView(device_, &view_info, nullptr, &view);
            made_view != VK_SUCCESS) {
            return vulkan_error(made_view, "creating a swapchain image view");
        }
        views_.push_back(view);
    }
    return {};
}

Result<Swapchain> make_swapchain(const Device& device, VkExtent2D window,
                                 const SwapchainOptions& options) {
    if (device.surface() == VK_NULL_HANDLE) {
        return make_error(ErrorCode::InvalidArgument,
                          "the device has no surface to show images on");
    }
    Swapchain swapchain;
    swapchain.physical_ = device.physical();
    swapchain.device_ = device.handle();
    swapchain.surface_ = device.surface();
    swapchain.options_ = options;
    if (auto built = swapchain.rebuild(window); !built) {
        return std::unexpected(std::move(built).error());
    }
    return swapchain;
}

} // namespace hog::render
