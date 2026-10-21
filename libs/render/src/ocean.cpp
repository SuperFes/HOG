#include <hog/core/assert.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/render/device.hpp>
#include <hog/render/ocean.hpp>
#include <hog/shaders/ocean.hpp>
#include <hog/terrain/sea.hpp>
#include <hog/waves/foam.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace hog::render {

namespace {

using terrain::SeaVertex;

static_assert(std::is_trivially_copyable_v<SeaVertex> && std::is_standard_layout_v<SeaVertex>);

// Triangles about as long as they're wide, a few pixels across at 1080p.
constexpr int rings = 96;
constexpr int spokes = 128;

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 up_height;
    glm::vec4 east_faked;
    glm::vec4 sun_nearest;
    glm::vec4 sky_furthest;
};

// Within what every device can push.
static_assert(sizeof(Push) <= 128);

constexpr std::array vertex_bindings{VkVertexInputBindingDescription{
    .binding = 0, .stride = sizeof(SeaVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
constexpr std::array vertex_attributes{
    VkVertexInputAttributeDescription{.location = 0,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32_SFLOAT,
                                      .offset = offsetof(SeaVertex, out)},
    VkVertexInputAttributeDescription{.location = 1,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32_SFLOAT,
                                      .offset = offsetof(SeaVertex, towards)}};

static_assert(sizeof(DrawnWaves) ==
              (5 * sizeof(glm::vec4)) + (drawn_waves * sizeof(DrawnWaves::Wave)));

constexpr VkDescriptorSetLayoutBinding
binding(std::uint32_t at, VkDescriptorType type,
        VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr};
}

// As ocean.slang has them.
constexpr std::uint32_t waves_binding = 6;
constexpr VkDescriptorSetLayoutBinding waves_descriptor =
    binding(waves_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

constexpr std::array descriptors{waves_descriptor};
constexpr std::array air_descriptors{binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                     binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                     binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                     binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                     binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                     binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                                     waves_descriptor};

} // namespace

DrawnWaves drawn_waves_at(std::span<const waves::Train> trains, std::span<const waves::Wave> waves,
                          const waves::SeaState& sea, WorldPosition eye, const terrain::Bend& bend,
                          GameTime time) {
    const double wind = glm::length(sea.wind);
    const auto breaking_slope = [&](const waves::Wave& wave) {
        const waves::Train& train = trains[wave.train];
        const double slope =
            train.wavenumber * wave.amplitude * waves::breaking_share(train.frequency, wind);
        return slope * slope / 2.0;
    };
    std::vector<waves::Wave> shown;
    double breaking_all = 0.0;
    for (const waves::Wave& wave : waves) {
        const double showing =
            std::sqrt(waves::showing(glm::dot(bend.up, trains[wave.train].heading)));
        if (showing > 0.0) {
            shown.push_back({.train = wave.train, .amplitude = wave.amplitude * showing});
            breaking_all += breaking_slope(shown.back());
        }
    }
    const waves::Strongest strongest = waves::strongest(trains, shown, drawn_waves);
    const planet::TangentFrame frame = planet::tangent_frame(bend.up);
    DrawnWaves drawn{.count_left_radius = {static_cast<float>(strongest.waves.size()),
                                           static_cast<float>(strongest.slope_left),
                                           static_cast<float>(bend.radius), 0.0F},
                     .up_height = {Vec3{bend.up}, static_cast<float>(bend.eye_height)},
                     .first = {Vec3{frame.east}, 0.0F},
                     .second = {Vec3{frame.north}, 0.0F}};
    double shorter = 0.0;
    double breaking_shorter = 0.0;
    for (std::size_t i = strongest.waves.size(); i-- > 0;) {
        const waves::Wave& wave = strongest.waves[i];
        const waves::Train& train = trains[wave.train];
        const DVec3 along = train.heading - (bend.up * glm::dot(bend.up, train.heading));
        const double flat = glm::length(along);
        const double wavenumber = train.wavenumber * flat;
        const double slope = wavenumber * wave.amplitude;
        const double share = waves::breaking_share(train.frequency, wind);
        shorter += slope * slope / 2.0;
        breaking_shorter += slope * slope * share * share / 2.0;
        breaking_all -= breaking_slope(wave);
        drawn.waves.at(i) = {
            .heading_wavenumber = {Vec3{train.heading}, static_cast<float>(train.wavenumber)},
            .amplitude_phase_slope_length =
                {static_cast<float>(wave.amplitude),
                 static_cast<float>(waves::phase_at(train, eye.metres(), time)),
                 static_cast<float>(shorter),
                 static_cast<float>(2.0 * std::numbers::pi / wavenumber)},
            .along = {static_cast<float>(glm::dot(along, frame.east) / flat),
                      static_cast<float>(glm::dot(along, frame.north) / flat),
                      static_cast<float>(wavenumber), 0.0F},
            .breaking = {static_cast<float>(share), static_cast<float>(breaking_shorter), 0.0F,
                         0.0F}};
    }
    // What's left over is what wasn't drawn; the bunching of all that breaks, drawn or not, is
    // near enough normal, so whitecaps cover as much as they should where it's highest.
    const double breaking_left = std::max(breaking_all, 0.0);
    const waves::Whitecaps white = waves::whitecaps(sea);
    const double breaking = breaking_shorter + breaking_left;
    if (white.breaking > 0.0 && breaking > 0.0) {
        drawn.foam = {static_cast<float>(std::sqrt(breaking) * waves::exceeded(white.breaking)),
                      static_cast<float>(breaking_left), static_cast<float>(white.breaking),
                      static_cast<float>(white.left)};
    }
    return drawn;
}

Result<Ocean> Ocean::create(const Device& device, VkFormat colour_format, VkFormat depth_format,
                            bool air, std::uint32_t frames) {
    Ocean ocean;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        auto waves = make_buffer(device, sizeof(DrawnWaves), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                 Memory::Upload);
        if (!waves) {
            return std::unexpected(std::move(waves).error());
        }
        ocean.waves_.push_back(std::move(waves).value());
    }
    const terrain::SeaDisc disc = terrain::sea_disc(rings, spokes);
    auto vertices = make_filled_buffer(device, std::as_bytes(std::span{disc.vertices}),
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (!vertices) {
        return std::unexpected(std::move(vertices).error());
    }
    ocean.vertices_ = std::move(vertices).value();
    auto indices = make_filled_buffer(device, std::as_bytes(std::span{disc.indices}),
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!indices) {
        return std::unexpected(std::move(indices).error());
    }
    ocean.indices_ = std::move(indices).value();
    ocean.index_count_ = static_cast<std::uint32_t>(disc.indices.size());

    // Seen from below too, from under the sea.
    ocean.air_ = air;
    auto pipeline = make_pipeline(
        device, {.spirv = shaders::ocean,
                 .fragment_entry = air ? "fragment_air" : "fragment_main",
                 .bindings = vertex_bindings,
                 .attributes = vertex_attributes,
                 .colour_format = colour_format,
                 .depth_format = depth_format,
                 .blend = Blend::Premultiplied,
                 .push_constant_bytes = sizeof(Push),
                 .descriptors = air ? std::span<const VkDescriptorSetLayoutBinding>{air_descriptors}
                                    : std::span<const VkDescriptorSetLayoutBinding>{descriptors}});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    ocean.pipeline_ = std::move(pipeline).value();
    if (air) {
        auto deep = make_pipeline(device, {.spirv = shaders::ocean,
                                           .fragment_entry = "fragment_deep",
                                           .bindings = vertex_bindings,
                                           .attributes = vertex_attributes,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .depth_write = false,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = air_descriptors});
        if (!deep) {
            return std::unexpected(std::move(deep).error());
        }
        ocean.deep_ = std::move(deep).value();
    }
    return ocean;
}

Result<> Ocean::draw(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                     std::uint32_t frame) const {
    return record(commands, extent, view, frame, pipeline_);
}

Result<> Ocean::draw_deep(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                          std::uint32_t frame) const {
    HOG_ASSERT(air_, "the deep water drawn without the air");
    // From under the sea, the sky shows through its surface overhead.
    if (view.bend.eye_height > 0.0) {
        return record(commands, extent, view, frame, deep_);
    }
    return {};
}

Result<> Ocean::record(VkCommandBuffer commands, VkExtent2D extent, const OceanView& view,
                       std::uint32_t frame, const Pipeline& pipeline) const {
    if (extent.width == 0 || extent.height == 0) {
        return {};
    }
    // A calm sea too lies on the planet round the eye.
    DrawnWaves drawn = view.waves;
    const planet::TangentFrame ground = planet::tangent_frame(view.bend.up);
    drawn.count_left_radius.z = static_cast<float>(view.bend.radius);
    drawn.up_height = {Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)};
    drawn.first = {Vec3{ground.east}, 0.0F};
    drawn.second = {Vec3{ground.north}, 0.0F};
    const Buffer& waves = waves_.at(frame);
    std::memcpy(waves.mapped().data(), &drawn, sizeof(drawn));
    if (auto flushed = waves.flush(); !flushed) {
        return flushed;
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    const terrain::SeaReach reach = terrain::sea_reach(view.bend);
    const Push push{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
                    .up_height =
                        glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
                    .east_faked = glm::vec4{Vec3{planet::tangent_frame(view.bend.up).east},
                                            static_cast<float>(view.bend.faked)},
                    .sun_nearest = glm::vec4{Vec3{view.sun}, static_cast<float>(reach.nearest)},
                    .sky_furthest = glm::vec4{view.sky, static_cast<float>(reach.furthest)}};

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    if (air_) {
        HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE,
                   "drawing the sea lit by the air without it");
        push_uniform_buffer(commands, pipeline, 0, view.air.air);
        push_uniform_buffer(commands, pipeline, 1, view.air.lighting);
        push_sampled_image(commands, pipeline, 2, view.air.transmittance, view.air.sampler);
        push_sampled_image(commands, pipeline, 3, view.air.sky_view, view.air.sampler);
        push_sampled_image(commands, pipeline, 4, view.air.aerial, view.air.sampler);
        push_sampled_image(commands, pipeline, 5, view.air.cloud_shadow, view.air.sampler);
    }
    push_uniform_buffer(commands, pipeline, waves_binding, waves.handle());
    const std::array vertices{vertices_.handle()};
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, vertices.data(), &offset);
    vkCmdBindIndexBuffer(commands, indices_.handle(), 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(commands, pipeline.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push),
                       &push);
    vkCmdDrawIndexed(commands, index_count_, 1, 0, 0, 0);
    return {};
}

} // namespace hog::render
