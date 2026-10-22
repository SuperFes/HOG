#include <hog/render/device.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/ripples.hpp>
#include <hog/shaders/ripples.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace hog::render {

namespace {

struct Push {
    glm::ivec4 shift{0};
    glm::vec4 timing{0.0F};
};

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr};
}

constexpr std::array descriptors{binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                                 binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
                                 binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                 binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                 binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)};

// Each column is stepped with the one holding the waves running the other way along the rows.
constexpr std::uint32_t column_pairs = (ripple_size / 2) + 1;

constexpr VkImageSubresourceRange whole{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .baseMipLevel = 0,
                                        .levelCount = 1,
                                        .baseArrayLayer = 0,
                                        .layerCount = 1};

Result<Pipeline> stage(const Device& device, const char* entry) {
    return make_compute_pipeline(device, {.spirv = shaders::ripples,
                                          .entry = entry,
                                          .push_constant_bytes = sizeof(Push),
                                          .descriptors = descriptors});
}

} // namespace

Result<RippleMap> RippleMap::create(Device& device) {
    RippleMap map;
    for (auto [pipeline, entry] :
         {std::pair{&map.rows_, "rows_main"}, std::pair{&map.columns_, "columns_main"},
          std::pair{&map.settle_, "settle_main"}}) {
        auto made = stage(device, entry);
        if (!made) {
            return std::unexpected(std::move(made).error());
        }
        *pipeline = std::move(made).value();
    }
    auto sampler = make_sampler(device, VK_FILTER_LINEAR);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    map.sampler_ = std::move(sampler).value();
    for (auto [image, use, usage] :
         {std::tuple{&map.state_, &map.state_use_,
                     VkImageUsageFlags{VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT}},
          std::tuple{&map.spectrum_, &map.spectrum_use_,
                     VkImageUsageFlags{VK_IMAGE_USAGE_STORAGE_BIT}}}) {
        auto made = make_image(device, {.format = VK_FORMAT_R32G32_SFLOAT,
                                        .extent = {.width = ripple_size, .height = ripple_size},
                                        .usage = usage});
        if (!made) {
            return std::unexpected(std::move(made).error());
        }
        *image = std::move(made).value();
        *use = TrackedImage{image->handle(), VK_IMAGE_ASPECT_COLOR_BIT};
    }
    if (auto cleared = device.submit_now([&](VkCommandBuffer commands) { map.clear(commands); });
        !cleared) {
        return std::unexpected(std::move(cleared).error());
    }
    return map;
}

void RippleMap::step(VkCommandBuffer commands, const GridStep& step, const SeaBindings& sea) {
    if (!step.kept) {
        clear(commands);
        return;
    }
    const int steps =
        std::clamp(static_cast<int>(std::ceil(step.seconds / ripple_step)), 1, ripple_most_steps);
    const double seconds = step.seconds / steps;
    for (int i = 0; i < steps; ++i) {
        record(commands, i == 0 ? step.shift : glm::ivec2{0}, seconds, sea);
    }
    state_use_.use_as(commands, image_use::sampled);
}

void RippleMap::clear(VkCommandBuffer commands) {
    const VkClearColorValue none{.float32 = {0.0F, 0.0F, 0.0F, 0.0F}};
    state_use_.use_as(commands, image_use::copy_target);
    vkCmdClearColorImage(commands, state_.handle(), image_use::copy_target.layout, &none, 1,
                         &whole);
    state_use_.use_as(commands, image_use::sampled);
}

void RippleMap::record(VkCommandBuffer commands, glm::ivec2 shift, double seconds,
                       const SeaBindings& sea) {
    const Push push{.shift = {shift, 1, 0},
                    .timing = {static_cast<float>(seconds), 0.0F, 0.0F, 0.0F}};
    for (const auto& [pipeline, groups] :
         {std::pair{&rows_, ripple_size}, std::pair{&columns_, column_pairs},
          std::pair{&settle_, ripple_size}}) {
        state_use_.use_as(commands, image_use::storage);
        spectrum_use_.use_as(commands, image_use::storage);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->handle());
        push_storage_image(commands, *pipeline, 0, state_.view());
        push_storage_image(commands, *pipeline, 1, spectrum_.view());
        push_uniform_buffer(commands, *pipeline, 2, sea.waves);
        push_sampled_image(commands, *pipeline, 3, sea.shore, sea.sampler);
        push_sampled_image(commands, *pipeline, 4, sea.fetches, sea.sampler);
        vkCmdPushConstants(commands, pipeline->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(push), &push);
        vkCmdDispatch(commands, groups, 1, 1);
    }
}

} // namespace hog::render
