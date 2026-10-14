#include <hog/render/device.hpp>
#include <hog/render/pipeline.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace hog::render {

Pipeline::Pipeline(Pipeline&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      set_layout_(std::exchange(other.set_layout_, VK_NULL_HANDLE)),
      layout_(std::exchange(other.layout_, VK_NULL_HANDLE)),
      pipeline_(std::exchange(other.pipeline_, VK_NULL_HANDLE)) {}

Pipeline& Pipeline::operator=(Pipeline&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        set_layout_ = std::exchange(other.set_layout_, VK_NULL_HANDLE);
        layout_ = std::exchange(other.layout_, VK_NULL_HANDLE);
        pipeline_ = std::exchange(other.pipeline_, VK_NULL_HANDLE);
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
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case Blend::Add:
        return VK_BLEND_FACTOR_ONE;
    }
    return VK_BLEND_FACTOR_ZERO;
}

} // namespace

Result<Pipeline> make_pipeline(const Device& device, const PipelineDesc& desc) {
    Pipeline pipeline;
    pipeline.device_ = device.handle();

    if (!desc.descriptors.empty()) {
        const VkDescriptorSetLayoutCreateInfo set_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = static_cast<std::uint32_t>(desc.descriptors.size()),
            .pBindings = desc.descriptors.data()};
        if (const VkResult made = vkCreateDescriptorSetLayout(device.handle(), &set_info, nullptr,
                                                              &pipeline.set_layout_);
            made != VK_SUCCESS) {
            return vulkan_error(made, "creating a descriptor set layout");
        }
    }

    const VkPushConstantRange push_range{.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                                                       VK_SHADER_STAGE_FRAGMENT_BIT,
                                         .offset = 0,
                                         .size = desc.push_constant_bytes};
    const VkPipelineLayoutCreateInfo layout_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = pipeline.set_layout_ != VK_NULL_HANDLE ? 1U : 0U,
        .pSetLayouts = &pipeline.set_layout_,
        .pushConstantRangeCount = desc.push_constant_bytes > 0 ? 1U : 0U,
        .pPushConstantRanges = &push_range};
    if (const VkResult made =
            vkCreatePipelineLayout(device.handle(), &layout_info, nullptr, &pipeline.layout_);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating a pipeline layout");
    }

    const VkShaderModuleCreateInfo module_info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                               .pNext = nullptr,
                                               .flags = 0,
                                               .codeSize = desc.spirv.size_bytes(),
                                               .pCode = desc.spirv.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    if (const VkResult made = vkCreateShaderModule(device.handle(), &module_info, nullptr, &module);
        made != VK_SUCCESS) {
        return vulkan_error(made, "creating a shader module");
    }
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
    vkCmdPushDescriptorSetKHR(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1,
                              &write);
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
    vkCmdPushDescriptorSetKHR(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1,
                              &write);
}

} // namespace hog::render
