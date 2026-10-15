#pragma once

#include <hog/render/device.hpp>
#include <hog/render/vulkan.hpp>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdint>

namespace hog::window {

// ImGui, drawing with Vulkan into images of `format`, with room for `textures` besides its font.
class Ui {
public:
    Ui(SDL_Window* window, const render::Device& device, VkFormat format,
       std::uint32_t textures = 0);
    Ui(const Ui&) = delete;
    Ui(Ui&&) = delete;
    Ui& operator=(const Ui&) = delete;
    Ui& operator=(Ui&&) = delete;
    ~Ui();

    [[nodiscard]] bool started() const noexcept { return vulkan_; }

private:
    bool vulkan_ = false;
};

// An image as ImGui samples it, in the layout fragment shaders read. The GPU must have finished
// with it before it goes.
class UiTexture {
public:
    explicit UiTexture(VkImageView view);
    UiTexture(const UiTexture&) = delete;
    UiTexture(UiTexture&&) = delete;
    UiTexture& operator=(const UiTexture&) = delete;
    UiTexture& operator=(UiTexture&&) = delete;
    ~UiTexture();

    [[nodiscard]] ImTextureRef ref() const noexcept;

private:
    VkDescriptorSet set_;
};

} // namespace hog::window
