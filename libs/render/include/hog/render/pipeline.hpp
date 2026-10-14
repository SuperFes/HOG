#pragma once

#include <hog/core/result.hpp>
#include <hog/render/vulkan.hpp>

#include <cstdint>
#include <span>
#include <string>

namespace hog::render {

class Device;

// A graphics pipeline drawing into colour and depth images by dynamic rendering, with the
// viewport and scissor set while recording.
struct PipelineDesc {
    // One SPIR-V module holding both entry points.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<const std::uint32_t> spirv = {};
    std::string vertex_entry = "vertex_main";
    std::string fragment_entry = "fragment_main";
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<const VkVertexInputBindingDescription> bindings = {};
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<const VkVertexInputAttributeDescription> attributes = {};
    VkFormat colour_format = VK_FORMAT_R8G8B8A8_UNORM;
    // VK_FORMAT_UNDEFINED for none. Depth is reversed, so nearer is greater.
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    // Visible to both stages.
    std::uint32_t push_constant_bytes = 0;
};

class Pipeline {
public:
    Pipeline() = default;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&& other) noexcept;
    Pipeline& operator=(Pipeline&& other) noexcept;
    ~Pipeline();

    [[nodiscard]] VkPipeline handle() const noexcept { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const noexcept { return layout_; }

private:
    friend Result<Pipeline> make_pipeline(const Device& device, const PipelineDesc& desc);
    void release() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

[[nodiscard]] Result<Pipeline> make_pipeline(const Device& device, const PipelineDesc& desc);

} // namespace hog::render
