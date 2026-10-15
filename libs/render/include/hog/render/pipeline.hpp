#pragma once

#include <hog/core/result.hpp>
#include <hog/render/vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace hog::render {

class Device;

// How a pipeline's colour combines with what's already drawn.
enum class Blend : std::uint8_t {
    None,
    Alpha,         // over what's there, by the colour's alpha
    Premultiplied, // added to what's there, which is kept by one less the colour's alpha
    Add,
    // What's there is kept by one less the colour, channel by channel: added light, tone mapped as
    // 1 - exp(-radiance) is, laid over light tone mapped the same way.
    Screen,
};

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
    // Whether drawing also writes depth, when there is a depth image; tested either way.
    bool depth_write = true;
    Blend blend = Blend::None;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    // Visible to both stages.
    std::uint32_t push_constant_bytes = 0;
    // What the shaders read from set 0, pushed while recording rather than allocated in sets.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<const VkDescriptorSetLayoutBinding> descriptors = {};
};

// A compute pipeline, its push constants and descriptors visible to the compute stage.
struct ComputePipelineDesc {
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<const std::uint32_t> spirv = {};
    std::string entry = "compute_main";
    std::uint32_t push_constant_bytes = 0;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    std::span<const VkDescriptorSetLayoutBinding> descriptors = {};
};

struct PipelineState;

class Pipeline {
public:
    Pipeline() noexcept;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&& other) noexcept;
    Pipeline& operator=(Pipeline&& other) noexcept;
    ~Pipeline();

    [[nodiscard]] VkPipeline handle() const noexcept;
    [[nodiscard]] VkPipelineLayout layout() const noexcept;
    [[nodiscard]] VkPipelineBindPoint bind_point() const noexcept;

private:
    friend Result<Pipeline> make_pipeline(const Device& device, const PipelineDesc& desc);
    friend Result<Pipeline> make_compute_pipeline(const Device& device,
                                                  const ComputePipelineDesc& desc);

    // Kept apart, so a pipeline moves without its registry hearing of it.
    std::unique_ptr<PipelineState> state_;
};

// The pipelines living on a device, so those made from a shader can be made again from new SPIR-V
// of it while running.
class PipelineRegistry {
public:
    explicit PipelineRegistry(VkDevice device) noexcept;
    PipelineRegistry(const PipelineRegistry&) = delete;
    PipelineRegistry& operator=(const PipelineRegistry&) = delete;
    PipelineRegistry(PipelineRegistry&&) = delete;
    PipelineRegistry& operator=(PipelineRegistry&&) = delete;
    ~PipelineRegistry();

    [[nodiscard]] std::size_t size() const;

    // Makes each pipeline first made from `made_from` again from `spirv`, which must keep the
    // shader's entry points, descriptors and push constants, waiting for the device to be idle
    // first. Where any fails, none change. Gives how many were made again.
    [[nodiscard]] Result<std::size_t> remake(std::span<const std::uint32_t> made_from,
                                             std::span<const std::uint32_t> spirv);

private:
    friend struct PipelineState;
    friend Result<Pipeline> make_pipeline(const Device& device, const PipelineDesc& desc);
    friend Result<Pipeline> make_compute_pipeline(const Device& device,
                                                  const ComputePipelineDesc& desc);
    void add(PipelineState& state);
    void remove(PipelineState* state) noexcept;

    VkDevice device_;
    mutable std::mutex mutex_;
    std::vector<PipelineState*> states_;
};

[[nodiscard]] Result<Pipeline> make_pipeline(const Device& device, const PipelineDesc& desc);
[[nodiscard]] Result<Pipeline> make_compute_pipeline(const Device& device,
                                                     const ComputePipelineDesc& desc);

// Records handing the bound pipeline's shaders an image to sample at `binding` of set 0. The image
// must be ready to sample by the time they run.
void push_sampled_image(VkCommandBuffer commands, const Pipeline& pipeline, std::uint32_t binding,
                        VkImageView view, VkSampler sampler) noexcept;
// As above, for a uniform buffer.
void push_uniform_buffer(VkCommandBuffer commands, const Pipeline& pipeline, std::uint32_t binding,
                         VkBuffer buffer) noexcept;
// As above, for an image to read and write in place, which must be in the storage layout.
void push_storage_image(VkCommandBuffer commands, const Pipeline& pipeline, std::uint32_t binding,
                        VkImageView view) noexcept;
// As above, for a buffer to read and write.
void push_storage_buffer(VkCommandBuffer commands, const Pipeline& pipeline, std::uint32_t binding,
                         VkBuffer buffer) noexcept;

} // namespace hog::render
