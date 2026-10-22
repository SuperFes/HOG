#include <hog/render/device.hpp>
#include <hog/render/ocean.hpp>
#include <hog/render/ripples.hpp>
#include <hog/shaders/ripples.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <tuple>
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
                                 binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                 binding(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)};

// As shaders/ripples.slang has them.
struct Bodies {
    // xy under the feet, texels; z how high they stand; w the radius.
    std::array<glm::vec4, ripple_most_bodies> feet{};
    // xyz from the feet to the top.
    std::array<glm::vec4, ripple_most_bodies> tall{};
    // x how many there are.
    glm::ivec4 count{0};
};

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

double rain_ring_chance(double falling, double snow) noexcept {
    const double rain = std::max(falling, 0.0) * std::clamp(1.0 - snow, 0.0, 1.0);
    return std::min(rain * rings_per_rain * rain_ring_cell * rain_ring_cell * rain_ring_life /
                        rain_ring_layers,
                    1.0);
}

double rain_ring_seconds(GameTime time) noexcept {
    // Short enough to time a ring to a hair in a float.
    constexpr double round = rain_ring_life * 5'000.0;
    return std::fmod(std::chrono::duration<double>(time.time_since_epoch()).count(), round);
}

Result<RippleMap> RippleMap::create(Device& device, std::uint32_t frames) {
    RippleMap map;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        auto bodies =
            make_buffer(device, sizeof(Bodies), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Memory::Upload);
        if (!bodies) {
            return std::unexpected(std::move(bodies).error());
        }
        map.bodies_.push_back(std::move(bodies).value());
    }
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

Result<> RippleMap::step(VkCommandBuffer commands, const GridStep& step, const SeaBindings& sea,
                         std::uint32_t frame, std::span<const RippleBody> bodies) {
    if (!step.kept) {
        clear(commands);
        return {};
    }
    Bodies laid;
    const std::size_t count = std::min(bodies.size(), ripple_most_bodies);
    for (std::size_t i = 0; i < count; ++i) {
        const RippleBody& body = bodies[i];
        laid.feet.at(i) = {body.feet, body.above, body.radius};
        laid.tall.at(i) = {body.tall, 0.0F};
    }
    laid.count.x = static_cast<int>(count);
    const Buffer& buffer = bodies_.at(frame);
    std::memcpy(buffer.mapped().data(), &laid, sizeof(laid));
    if (auto flushed = buffer.flush(); !flushed) {
        return flushed;
    }
    const int steps =
        std::clamp(static_cast<int>(std::ceil(step.seconds / ripple_step)), 1, ripple_most_steps);
    const double seconds = step.seconds / steps;
    for (int i = 0; i < steps; ++i) {
        record(commands, i == 0 ? step.shift : glm::ivec2{0}, seconds, sea, buffer.handle());
    }
    state_use_.use_as(commands, image_use::sampled);
    return {};
}

void RippleMap::clear(VkCommandBuffer commands) {
    const VkClearColorValue none{.float32 = {0.0F, 0.0F, 0.0F, 0.0F}};
    state_use_.use_as(commands, image_use::copy_target);
    vkCmdClearColorImage(commands, state_.handle(), image_use::copy_target.layout, &none, 1,
                         &whole);
    state_use_.use_as(commands, image_use::sampled);
}

void RippleMap::record(VkCommandBuffer commands, glm::ivec2 shift, double seconds,
                       const SeaBindings& sea, VkBuffer bodies) {
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
        push_uniform_buffer(commands, *pipeline, 5, bodies);
        vkCmdPushConstants(commands, pipeline->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(push), &push);
        vkCmdDispatch(commands, groups, 1, 1);
    }
}

} // namespace hog::render
