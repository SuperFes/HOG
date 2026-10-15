#pragma once

#include <hog/core/result.hpp>
#include <hog/render/device.hpp>
#include <hog/render/vulkan.hpp>

#include <SDL3/SDL.h>

#include <memory>

namespace hog::window {

// What test runners take as skipped: there's no Vulkan to draw with.
inline constexpr int no_vulkan = 77;

// SDL from starting to quitting, with Vulkan loaded for its windows.
class Sdl {
public:
    Sdl();
    Sdl(const Sdl&) = delete;
    Sdl(Sdl&&) = delete;
    Sdl& operator=(const Sdl&) = delete;
    Sdl& operator=(Sdl&&) = delete;
    ~Sdl();

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool vulkan() const noexcept { return vulkan_; }

private:
    bool started_;
    bool vulkan_;
};

struct WindowDeleter {
    void operator()(SDL_Window* window) const noexcept { SDL_DestroyWindow(window); }
};
using Window = std::unique_ptr<SDL_Window, WindowDeleter>;

// A device for the window: `device` with the instance extensions SDL needs, and making its
// surface once there's an instance to make it with.
[[nodiscard]] Result<render::DeviceOptions> device_options(SDL_Window* window,
                                                           render::DeviceOptions device);

// The window's drawable size, which is none while it's minimised.
[[nodiscard]] VkExtent2D drawable(SDL_Window* window);

} // namespace hog::window
