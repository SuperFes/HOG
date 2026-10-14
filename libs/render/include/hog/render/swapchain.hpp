#pragma once

#include <hog/core/result.hpp>
#include <hog/render/vulkan.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace hog::render {

class Device;

struct SwapchainOptions {
    // Waits for the display's refresh rather than tearing or dropping frames.
    bool vsync = true;
    // Images that encode linear colour to sRGB as they're written.
    bool srgb = true;
};

[[nodiscard]] VkSurfaceFormatKHR
choose_surface_format(std::span<const VkSurfaceFormatKHR> available, bool srgb) noexcept;
[[nodiscard]] VkPresentModeKHR choose_present_mode(std::span<const VkPresentModeKHR> available,
                                                   bool vsync) noexcept;
// The surface's own size where it has one, otherwise the window's, within what it allows. Zero
// while the window is minimised.
[[nodiscard]] VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& capabilities,
                                       VkExtent2D window) noexcept;
// One more than the least, so a frame can be drawn while the display holds the others.
[[nodiscard]] std::uint32_t
choose_image_count(const VkSurfaceCapabilitiesKHR& capabilities) noexcept;

// Whether an acquire or present says the swapchain no longer fits its surface, or an error for any
// other failure. A suboptimal image is still drawn and shown first.
[[nodiscard]] Result<bool> swapchain_stale(VkResult result, std::string_view doing);

// The images shown on the device's surface.
class Swapchain {
public:
    Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    Swapchain(Swapchain&& other) noexcept;
    Swapchain& operator=(Swapchain&& other) noexcept;
    ~Swapchain();

    // Replaces the images to fit the window. The GPU must have finished with the old ones. While
    // the surface has no area there are no images.
    Result<> rebuild(VkExtent2D window);

    [[nodiscard]] VkSwapchainKHR handle() const noexcept { return swapchain_; }
    [[nodiscard]] VkFormat format() const noexcept { return format_; }
    [[nodiscard]] VkExtent2D extent() const noexcept { return extent_; }
    // Always a colour target; a copy source and target where the surface allows.
    [[nodiscard]] VkImageUsageFlags usage() const noexcept { return usage_; }
    [[nodiscard]] std::span<const VkImage> images() const noexcept { return images_; }
    [[nodiscard]] std::span<const VkImageView> views() const noexcept { return views_; }

private:
    friend Result<Swapchain> make_swapchain(const Device& device, VkExtent2D window,
                                            const SwapchainOptions& options);
    void release_views() noexcept;
    void release() noexcept;

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    SwapchainOptions options_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkImageUsageFlags usage_ = 0;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
};

[[nodiscard]] Result<Swapchain> make_swapchain(const Device& device, VkExtent2D window,
                                               const SwapchainOptions& options);

} // namespace hog::render
