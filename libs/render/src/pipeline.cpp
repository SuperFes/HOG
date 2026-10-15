#include <hog/render/device.hpp>
#include <hog/render/pipeline.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

namespace hog::render {

Pipeline::Pipeline(Pipeline&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      set_layout_(std::exchange(other.set_layout_, VK_NULL_HANDLE)),
      layout_(std::exchange(other.layout_, VK_NULL_HANDLE)),
      pipeline_(std::exchange(other.pipeline_, VK_NULL_HANDLE)), bind_point_(other.bind_point_) {}

Pipeline& Pipeline::operator=(Pipeline&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        set_layout_ = std::exchange(other.set_layout_, VK_NULL_HANDLE);
        layout_ = std::exchange(other.layout_, VK_NULL_HANDLE);
        pipeline_ = std::exchange(other.pipeline_, VK_NULL_HANDLE);
        bind_point_ = other.bind_point_;
    }
    return *this;
}

Pipeline::~Pipeline() {
    release();
}

void Pipeline::release() noexcept {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    if (set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        set_layout_ = VK_NULL_HANDLE;
    }
}

namespace {

// How much of what's already drawn a blend keeps.
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

} // namespace

Result<Pipeline> make_pipeline(const Device& device, const PipelineDesc& desc) {
    Pipeline pipeline;
    pipeline.device_ = device.handle();
    pipeline.bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (auto made = make_layouts(device.handle(), desc.descriptors, desc.push_constant_bytes,
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                 pipeline.set_layout_, pipeline.layout_);
        !made) {
        return std::unexpected(std::move(made).error());
    }
    const auto made_module = make_module(device.handle(), desc.spirv);
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
        .layout = pipeline.layout_,
        .renderPass = VK_NULL_HANDLE,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1};
    const VkResult made = vkCreateGraphicsPipelines(device.handle(), VK_NULL_HANDLE, 1,
                                                    &pipeline_info, nullptr, &pipeline.pipeline_);
    vkDestroyShaderModule(device.handle(), module, nullptr);
    if (made != VK_SUCCESS) {
        return vulkan_error(made, "creating a graphics pipeline");
    }
    return pipeline;
}

Result<Pipeline> make_compute_pipeline(const Device& device, const ComputePipelineDesc& desc) {
    Pipeline pipeline;
    pipeline.device_ = device.handle();
    pipeline.bind_point_ = VK_PIPELINE_BIND_POINT_COMPUTE;
    if (auto made =
            make_layouts(device.handle(), desc.descriptors, desc.push_constant_bytes,
                         VK_SHADER_STAGE_COMPUTE_BIT, pipeline.set_layout_, pipeline.layout_);
        !made) {
        return std::unexpected(std::move(made).error());
    }
    const auto module = make_module(device.handle(), desc.spirv);
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
        .layout = pipeline.layout_,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1};
    const VkResult made = vkCreateComputePipelines(device.handle(), VK_NULL_HANDLE, 1,
                                                   &pipeline_info, nullptr, &pipeline.pipeline_);
    vkDestroyShaderModule(device.handle(), *module, nullptr);
    if (made != VK_SUCCESS) {
        return vulkan_error(made, "creating a compute pipeline");
    }
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
    const VkDescriptorBufferInfo whole{.buffer = buffer, .offset = 0, .range = VK_WHOLE_SIZE};
    const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                     .pNext = nullptr,
                                     .dstSet = VK_NULL_HANDLE,
                                     .dstBinding = binding,
                                     .dstArrayElement = 0,
                                     .descriptorCount = 1,
                                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                     .pImageInfo = nullptr,
                                     .pBufferInfo = &whole,
                                     .pTexelBufferView = nullptr};
    push_descriptor(commands, pipeline, write);
}

} // namespace hog::render
