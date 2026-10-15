#include <hog/render/presenter.hpp>
#include <hog/window/ui.hpp>

#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <cstdint>

namespace hog::window {

Ui::Ui(SDL_Window* window, const render::Device& device, VkFormat format, std::uint32_t textures) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForVulkan(window);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = device.instance();
    info.PhysicalDevice = device.physical();
    info.Device = device.handle();
    info.QueueFamily = device.queue_family();
    info.Queue = device.queue();
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + textures;
    // Its vertex buffers are reused once a frame in flight finishes, not a swapchain image.
    info.MinImageCount = render::frames_in_flight;
    info.ImageCount = render::frames_in_flight;
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
        .depthAttachmentFormat = VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED};
    vulkan_ = ImGui_ImplVulkan_Init(&info);
}

Ui::~Ui() {
    if (vulkan_) {
        ImGui_ImplVulkan_Shutdown();
    }
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

UiTexture::UiTexture(VkImageView view)
    : set_{ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)} {}

UiTexture::~UiTexture() {
    ImGui_ImplVulkan_RemoveTexture(set_);
}

ImTextureRef UiTexture::ref() const noexcept {
    // The Vulkan backend takes descriptor sets as ImGui's opaque ids.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return ImTextureRef{static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(set_))};
}

} // namespace hog::window
