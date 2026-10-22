#include <hog/render/device.hpp>
#include <hog/render/foam.hpp>
#include <hog/render/ocean.hpp>
#include <hog/shaders/foam.hpp>

#include <array>
#include <cstdint>
#include <utility>

namespace hog::render {

namespace {

struct Push {
    glm::ivec4 shift{0};
    glm::vec4 timing{0.0F};
    glm::vec4 drift{0.0F};
};

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr};
}

constexpr std::array descriptors{binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                 binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                                 binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                 binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                 binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};

constexpr std::uint32_t group = 8;

} // namespace

Result<FoamMap> FoamMap::create(Device& device) {
    FoamMap map;
    auto pipeline = make_compute_pipeline(
        device,
        {.spirv = shaders::foam, .push_constant_bytes = sizeof(Push), .descriptors = descriptors});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    map.pipeline_ = std::move(pipeline).value();
    auto sampler = make_sampler(device, VK_FILTER_LINEAR);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    map.sampler_ = std::move(sampler).value();
    for (std::size_t i = 0; i < map.images_.size(); ++i) {
        auto image = make_image(
            device, {.format = VK_FORMAT_R32G32B32A32_SFLOAT,
                     .extent = {.width = foam_size, .height = foam_size},
                     .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT});
        if (!image) {
            return std::unexpected(std::move(image).error());
        }
        map.images_.at(i) = std::move(image).value();
        map.uses_.at(i) = TrackedImage{map.images_.at(i).handle(), VK_IMAGE_ASPECT_COLOR_BIT};
    }
    if (auto cleared = device.submit_now([&](VkCommandBuffer commands) {
            const VkClearColorValue none{.float32 = {0.0F, 0.0F, 0.0F, 0.0F}};
            const VkImageSubresourceRange whole{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                .baseMipLevel = 0,
                                                .levelCount = 1,
                                                .baseArrayLayer = 0,
                                                .layerCount = 1};
            for (std::size_t i = 0; i < map.images_.size(); ++i) {
                map.uses_.at(i).use_as(commands, image_use::copy_target);
                vkCmdClearColorImage(commands, map.images_.at(i).handle(),
                                     image_use::copy_target.layout, &none, 1, &whole);
                map.uses_.at(i).use_as(commands, image_use::sampled);
            }
        });
        !cleared) {
        return std::unexpected(std::move(cleared).error());
    }
    return map;
}

void FoamMap::step(VkCommandBuffer commands, const GridStep& step, const SeaBindings& sea,
                   DVec3 drift, bool catch_up) {
    if (step.kept) {
        record(commands, step, sea, drift, 0.0);
        return;
    }
    if (!catch_up) {
        record(commands, {.shift = glm::ivec2{0}, .seconds = 0.0, .kept = false}, sea, drift, 0.0);
        return;
    }
    for (int back = foam_catch_up; back >= 0; --back) {
        record(
            commands,
            {.shift = glm::ivec2{0}, .seconds = foam_catch_up_step, .kept = back < foam_catch_up},
            sea, drift, back * foam_catch_up_step);
    }
}

void FoamMap::record(VkCommandBuffer commands, const GridStep& step, const SeaBindings& sea,
                     DVec3 drift, double ago) {
    const std::size_t next = 1 - latest_;
    uses_.at(latest_).use_as(commands, image_use::sampled);
    uses_.at(next).use_as(commands, image_use::storage);
    const Push push{
        .shift = {step.shift, step.kept ? 1 : 0, 0},
        .timing = {static_cast<float>(step.seconds), static_cast<float>(ago), 0.0F, 0.0F},
        .drift = {Vec3{drift}, 0.0F}};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.handle());
    push_sampled_image(commands, pipeline_, 0, images_.at(latest_).view(), sampler_.handle());
    push_storage_image(commands, pipeline_, 1, images_.at(next).view());
    push_uniform_buffer(commands, pipeline_, 2, sea.waves);
    push_sampled_image(commands, pipeline_, 3, sea.shore, sea.sampler);
    push_sampled_image(commands, pipeline_, 4, sea.fetches, sea.sampler);
    vkCmdPushConstants(commands, pipeline_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                       &push);
    vkCmdDispatch(commands, foam_size / group, foam_size / group, 1);
    uses_.at(next).use_as(commands, image_use::sampled);
    latest_ = next;
}

} // namespace hog::render
