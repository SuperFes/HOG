#pragma once

#include <hog/core/result.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/swapchain.hpp>
#include <hog/render/vulkan.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hog::render {

class Device;

// The CPU records one frame while the GPU draws the one before.
inline constexpr std::uint32_t frames_in_flight = 2;

// One frame being recorded into a swapchain image.
struct Frame {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    // Starts with no contents; whatever use it's left in, it's shown from.
    TrackedImage target;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    // Which frame in flight this is, for resources kept one per frame.
    std::uint32_t slot = 0;
    std::uint32_t image = 0;
};

// Whether the swapchain must be rebuilt before the next frame: it said it was stale, or the window
// is no longer the size it was built for.
[[nodiscard]] bool needs_rebuild(bool stale, VkExtent2D built_for, VkExtent2D window) noexcept;

// Draws frames into the device's surface, rebuilding the swapchain as the window changes.
class Presenter {
public:
    [[nodiscard]] static Result<std::unique_ptr<Presenter>>
    create(Device& device, VkExtent2D window, const SwapchainOptions& options = {});

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    Presenter(Presenter&&) = delete;
    Presenter& operator=(Presenter&&) = delete;
    ~Presenter();

    // Waits for this slot's last frame, takes the next image and begins recording. Empty when
    // there's nothing to draw into, such as while the window is minimised.
    [[nodiscard]] Result<std::optional<Frame>> begin(VkExtent2D window);
    // Readies the image to be shown, submits the frame and shows it.
    Result<> end(Frame& frame);

    [[nodiscard]] const Swapchain& swapchain() const noexcept { return swapchain_; }

private:
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE;
    };

    explicit Presenter(Device& device) : device_{&device} {}
    Result<> rebuild(VkExtent2D window);

    Device* device_;
    Swapchain swapchain_;
    std::array<Slot, frames_in_flight> slots_{};
    // One per swapchain image: presenting has no fence, so a semaphore can only be reused once the
    // same image comes round again.
    std::vector<VkSemaphore> rendered_;
    VkExtent2D built_for_{};
    bool stale_ = false;
    std::uint32_t slot_ = 0;
};

} // namespace hog::render
