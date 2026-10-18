#include <hog/render/device.hpp>
#include <hog/render/pipeline.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace hog::render {

// A pipeline's handles, and what it was made with, so it can be made again from new SPIR-V.
struct PipelineState {
    PipelineState() = default;
    PipelineState(const PipelineState&) = delete;
    PipelineState& operator=(const PipelineState&) = delete;
    PipelineState(PipelineState&&) = delete;
    PipelineState& operator=(PipelineState&&) = delete;
    ~PipelineState();

    PipelineRegistry* registry = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    const std::uint32_t* made_from = nullptr; // the SPIR-V it was first made from
    // Without its SPIR-V, and its spans into the copies below.
    std::variant<PipelineDesc, ComputePipelineDesc> desc;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkDescriptorSetLayoutBinding> descriptors;
};

PipelineState::~PipelineState() {
    if (registry != nullptr) {
        registry->remove(this);
    }
    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, layout, nullptr);
    }
    if (set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    }
}

Pipeline::Pipeline() noexcept = default;
Pipeline::Pipeline(Pipeline&& other) noexcept = default;
Pipeline& Pipeline::operator=(Pipeline&& other) noexcept = default;
Pipeline::~Pipeline() = default;

VkPipeline Pipeline::handle() const noexcept {
    return state_ ? state_->pipeline : VK_NULL_HANDLE;
}

VkPipelineLayout Pipeline::layout() const noexcept {
    return state_ ? state_->layout : VK_NULL_HANDLE;
}

VkPipelineBindPoint Pipeline::bind_point() const noexcept {
    return state_ ? state_->bind_point : VK_PIPELINE_BIND_POINT_GRAPHICS;
}

namespace {

VkBlendFactor kept_colour(Blend blend) noexcept {
    switch (blend) {
    case Blend::None:
        return VK_BLEND_FACTOR_ZERO;
    case Blend::Alpha:
    case Blend::Premultiplied:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case Blend::Add:
        return VK_BLEND_FACTOR_ONE;
    case Blend::Screen:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    }
    return VK_BLEND_FACTOR_ZERO;
}

// Set 0 of push descriptors, if any, and the push constants, visible to `stages`.
Result<> make_layouts(VkDevice device, std::span<const VkDescriptorSetLayoutBinding> descriptors,
                      std::uint32_t push_constant_bytes, VkShaderStageFlags stages,
                      VkDescriptorSetLayout& set_layout, VkPipelineLayout& layout) {
    if (!descriptors.empty()) {
        const VkDescriptorSetLayoutCreateInfo set_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = static_cast<std::uint32_t>(descriptors.size()),
            .pBindings = descriptors.data()};
        if (const VkResult made =
                vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout);
            made != VK_SUCCESS) {
            return vulkan_error(made, "creating a descriptor set layout");
        }
    }

    const VkPushConstantRange push_range{
        .stageFlags = stages, .offset = 0, .size = push_constant_bytes};
    const VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = set_layout != VK_NULL_HANDLE ? 1U : 0U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = push_constant_bytes > 0 ? 1U : 0U,
        .pPushConstantRanges = &push_range};
    if (const VkResult made = vkCreatePipelineLayout(device, &layout_info, nullptr, &layout);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating a pipeline layout");
    }
    return {};
}

Result<VkShaderModule> make_module(VkDevice device, std::span<const std::uint32_t> spirv) {
    const VkShaderModuleCreateInfo module_info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                               .pNext = nullptr,
                                               .flags = 0,
                                               .codeSize = spirv.size_bytes(),
                                               .pCode = spirv.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    if (const VkResult made = vkCreateShaderModule(device, &module_info, nullptr, &module);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating a shader module");
    }
    return module;
}

void push_descriptor(VkCommandBuffer commands, const Pipeline& pipeline,
                     const VkWriteDescriptorSet& write) noexcept {
    vkCmdPushDescriptorSetKHR(commands, pipeline.bind_point(), pipeline.layout(), 0, 1, &write);
}

Result<VkPipeline> build_graphics(VkDevice device, const PipelineDesc& desc,
                                  std::span<const std::uint32_t> spirv, VkPipelineLayout layout) {
    const auto made_module = make_module(device, spirv);
    if (!made_module) {
        return std::unexpected(made_module.error());
    }
    VkShaderModule module = *made_module;
    const std::array stages{VkPipelineShaderStageCreateInfo{
                                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                .pNext = nullptr,
                                .flags = 0,
                                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                .module = module,
                                .pName = desc.vertex_entry.c_str(),
                                .pSpecializationInfo = nullptr},
                            VkPipelineShaderStageCreateInfo{
                                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                .pNext = nullptr,
                                .flags = 0,
                                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                .module = module,
                                .pName = desc.fragment_entry.c_str(),
                                .pSpecializationInfo = nullptr}};

    const VkPipelineVertexInputStateCreateInfo vertex_input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = static_cast<std::uint32_t>(desc.bindings.size()),
        .pVertexBindingDescriptions = desc.bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(desc.attributes.size()),
        .pVertexAttributeDescriptions = desc.attributes.data()};
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE};
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr,
        .scissorCount = 1,
        .pScissors = nullptr};
    const VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = desc.cull,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0F,
        .depthBiasClamp = 0.0F,
        .depthBiasSlopeFactor = 0.0F,
        .lineWidth = 1.0F};
    const VkPipelineMultisampleStateCreateInfo multisample{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0.0F,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE};
    const bool depth = desc.depth_format != VK_FORMAT_UNDEFINED;
    const VkPipelineDepthStencilStateCreateInfo depth_stencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = depth ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = depth && desc.depth_write ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_GREATER,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
        .front = {},
        .back = {},
        .minDepthBounds = 0.0F,
        .maxDepthBounds = 1.0F};
    const bool blending = desc.blend != Blend::None;
    const VkPipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = blending ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor =
            desc.blend == Blend::Alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = kept_colour(desc.blend),
        .colorBlendOp = VK_BLEND_OP_ADD,
        // What's drawn over keeps its own alpha.
        .srcAlphaBlendFactor = blending ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = blending ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    const VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
        .blendConstants = {}};
    constexpr std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data()};
    const VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &desc.colour_format,
        .depthAttachmentFormat = desc.depth_format,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED};
    const VkGraphicsPipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .flags = 0,
        .stageCount = static_cast<std::uint32_t>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pTessellationState = nullptr,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = layout,
        .renderPass = VK_NULL_HANDLE,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1};
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult made =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (made != VK_SUCCESS) {
        return vulkan_error(made, "creating a graphics pipeline");
    }
    return pipeline;
}

Result<VkPipeline> build_compute(VkDevice device, const ComputePipelineDesc& desc,
                                 std::span<const std::uint32_t> spirv, VkPipelineLayout layout) {
    const auto module = make_module(device, spirv);
    if (!module) {
        return std::unexpected(module.error());
    }
    const VkComputePipelineCreateInfo pipeline_info{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .pNext = nullptr,
                  .flags = 0,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = *module,
                  .pName = desc.entry.c_str(),
                  .pSpecializationInfo = nullptr},
        .layout = layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1};
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult made =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
    vkDestroyShaderModule(device, *module, nullptr);
    if (made != VK_SUCCESS) {
        return vulkan_error(made, "creating a compute pipeline");
    }
    return pipeline;
}

Result<VkPipeline> build(const PipelineState& state, std::span<const std::uint32_t> spirv) {
    if (const auto* graphics = std::get_if<PipelineDesc>(&state.desc)) {
        return build_graphics(state.device, *graphics, spirv, state.layout);
    }
    return build_compute(state.device, std::get<ComputePipelineDesc>(state.desc), spirv,
                         state.layout);
}

// Copies what a description points to into the state, which never moves, so it outlasts the
// caller's.
void keep(PipelineState& state, const PipelineDesc& desc) {
    state.bindings.assign(desc.bindings.begin(), desc.bindings.end());
    state.attributes.assign(desc.attributes.begin(), desc.attributes.end());
    state.descriptors.assign(desc.descriptors.begin(), desc.descriptors.end());
    PipelineDesc kept = desc;
    kept.spirv = {};
    kept.bindings = state.bindings;
    kept.attributes = state.attributes;
    kept.descriptors = state.descriptors;
    state.desc = std::move(kept);
}

void keep(PipelineState& state, const ComputePipelineDesc& desc) {
    state.descriptors.assign(desc.descriptors.begin(), desc.descriptors.end());
    ComputePipelineDesc kept = desc;
    kept.spirv = {};
    kept.descriptors = state.descriptors;
    state.desc = std::move(kept);
}

template <typename Desc>
Result<std::unique_ptr<PipelineState>>
make(VkDevice device, const Desc& desc, VkPipelineBindPoint bind_point, VkShaderStageFlags stages) {
    auto state = std::make_unique<PipelineState>();
    state->device = device;
    state->bind_point = bind_point;
    state->made_from = desc.spirv.data();
    keep(*state, desc);
    if (auto made = make_layouts(device, desc.descriptors, desc.push_constant_bytes, stages,
                                 state->set_layout, state->layout);
        !made) {
        return std::unexpected(std::move(made).error());
    }
    auto pipeline = build(*state, desc.spirv);
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    state->pipeline = *pipeline;
    return state;
}

} // namespace

PipelineRegistry::PipelineRegistry(VkDevice device) noexcept : device_(device) {}

PipelineRegistry::~PipelineRegistry() {
    // Pipelines outliving their device are a bug, but needn't crash.
    for (PipelineState* state : states_) {
        state->registry = nullptr;
    }
}

void PipelineRegistry::add(PipelineState& state) {
    const std::scoped_lock lock{mutex_};
    states_.push_back(&state);
    state.registry = this;
}

void PipelineRegistry::remove(PipelineState* state) noexcept {
    const std::scoped_lock lock{mutex_};
    std::erase(states_, state);
}

std::size_t PipelineRegistry::size() const {
    const std::scoped_lock lock{mutex_};
    return states_.size();
}

Result<std::size_t> PipelineRegistry::remake(std::span<const std::uint32_t> made_from,
                                             std::span<const std::uint32_t> spirv) {
    // Drivers needn't survive what isn't SPIR-V at all.
    constexpr std::uint32_t magic = 0x07230203;
    constexpr std::size_t header_words = 5;
    if (spirv.size() < header_words || spirv.front() != magic) {
        return make_error(ErrorCode::InvalidArgument,
                          "making pipelines again from what isn't SPIR-V");
    }
    const std::scoped_lock lock{mutex_};
    std::vector<std::pair<PipelineState*, VkPipeline>> remade;
    for (PipelineState* state : states_) {
        if (state->made_from != made_from.data()) {
            continue;
        }
        auto pipeline = build(*state, spirv);
        if (!pipeline) {
            for (const auto& [kept, made] : remade) {
                vkDestroyPipeline(device_, made, nullptr);
            }
            return std::unexpected(std::move(pipeline).error());
        }
        remade.emplace_back(state, *pipeline);
    }
    if (remade.empty()) {
        return 0;
    }
    // The old ones may be in commands still running.
    vkDeviceWaitIdle(device_);
    for (const auto& [state, made] : remade) {
        vkDestroyPipeline(device_, std::exchange(state->pipeline, made), nullptr);
    }
    return remade.size();
}

Result<Pipeline> make_pipeline(const Device& device, const PipelineDesc& desc) {
    auto state = make(device.handle(), desc, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    if (!state) {
        return std::unexpected(std::move(state).error());
    }
    device.pipelines().add(**state);
    Pipeline pipeline;
    pipeline.state_ = std::move(state).value();
    return pipeline;
}

Result<Pipeline> make_compute_pipeline(const Device& device, const ComputePipelineDesc& desc) {
    auto state =
        make(device.handle(), desc, VK_PIPELINE_BIND_POINT_COMPUTE, VK_SHADER_STAGE_COMPUTE_BIT);
    if (!state) {
        return std::unexpected(std::move(state).error());
    }
    device.pipelines().add(**state);
    Pipeline pipeline;
    pipeline.state_ = std::move(state).value();
    return pipeline;
}

void push_sampled_image(VkCommandBuffer commands, const Pipeline& pipeline, std::uint32_t binding,
                        VkImageView view, VkSampler sampler) noexcept {
    const VkDescriptorImageInfo image{.sampler = sampler,
                                      .imageView = view,
                                      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .pNext = nullptr,
                                     .dstSet = VK_NULL_HANDLE,
                                     .dstBinding = binding,
                                     .dstArrayElement = 0,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     .pImageInfo = &image,
                                     .pBufferInfo = nullptr,
                                     .pTexelBufferView = nullptr};
    push_descriptor(commands, pipeline, write);
}

void push_uniform_buffer(VkCommandBuffer commands, const Pipeline& pipeline, std::uint32_t binding,
                         VkBuffer buffer) noexcept {
    const VkDescriptorBufferInfo whole{.buffer = buffer, .offset = 0, .range = VK_WHOLE_SIZE};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .pNext = nullptr,
                                     .dstSet = VK_NULL_HANDLE,
                                     .dstBinding = binding,
                                     .dstArrayElement = 0,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                     .pImageInfo = nullptr,
                                     .pBufferInfo = &whole,
                                     .pTexelBufferView = nullptr};
    push_descriptor(commands, pipeline, write);
}

void push_storage_image(VkCommandBuffer commands, const Pipeline& pipeline, std::uint32_t binding,
                        VkImageView view) noexcept {
    const VkDescriptorImageInfo image{
        .sampler = VK_NULL_HANDLE, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .pNext = nullptr,
                                     .dstSet = VK_NULL_HANDLE,
                                     .dstBinding = binding,
                                     .dstArrayElement = 0,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     .pImageInfo = &image,
                                     .pBufferInfo = nullptr,
                                     .pTexelBufferView = nullptr};
    push_descriptor(commands, pipeline, write);
}

void push_storage_buffer(VkCommandBuffer commands, const Pipeline& pipeline, std::uint32_t binding,
                         VkBuffer buffer) noexcept {
    push_storage_buffer(commands, pipeline, binding, buffer, 0, VK_WHOLE_SIZE);
}

void push_storage_buffer(VkCommandBuffer commands, const Pipeline& pipeline, std::uint32_t binding,
                         VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range) noexcept {
    const VkDescriptorBufferInfo part{.buffer = buffer, .offset = offset, .range = range};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .pNext = nullptr,
                                     .dstSet = VK_NULL_HANDLE,
                                     .dstBinding = binding,
                                     .dstArrayElement = 0,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     .pImageInfo = nullptr,
                                     .pBufferInfo = &part,
                                     .pTexelBufferView = nullptr};
    push_descriptor(commands, pipeline, write);
}

} // namespace hog::render
