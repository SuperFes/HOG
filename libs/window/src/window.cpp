#include <hog/window/window.hpp>

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <span>
#include <utility>

namespace hog::window {

Sdl::Sdl()
    : started_(SDL_Init(SDL_INIT_VIDEO)), vulkan_(started_ && SDL_Vulkan_LoadLibrary(nullptr)) {}

Sdl::~Sdl() {
    if (vulkan_) {
        SDL_Vulkan_UnloadLibrary();
    }
    if (started_) {
        SDL_Quit();
    }
}

Result<render::DeviceOptions> device_options(SDL_Window* window, render::DeviceOptions device) {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (names == nullptr) {
        return make_error(
            ErrorCode::Unsupported,
            std::format("can't use Vulkan on this window system: {}", SDL_GetError()));
    }
    for (const char* name : std::span{names, count}) {
        device.instance_extensions.emplace_back(name);
    }
    device.make_surface = [window](VkInstance instance) -> Result<VkSurfaceKHR> {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
            return make_error(ErrorCode::Unsupported,
                              std::format("can't show Vulkan on the window: {}", SDL_GetError()));
        }
        return surface;
    };
    return device;
}

VkExtent2D drawable(SDL_Window* window) {
    int width = 0;
    int height = 0;
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0 ||
        !SDL_GetWindowSizeInPixels(window, &width, &height)) {
        return {};
    }
    return {.width = static_cast<std::uint32_t>(std::max(width, 0)),
            .height = static_cast<std::uint32_t>(std::max(height, 0))};
}

} // namespace hog::window
