#include <hog/core/assert.hpp>
#include <hog/planet/climate.hpp>
#include <hog/render/device.hpp>
#include <hog/render/grass.hpp>
#include <hog/shaders/grass.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <format>
#include <utility>

namespace hog::render {

namespace {

// The shader reads the pool's vertices by this layout.
static_assert(sizeof(terrain::PatchVertex) == 28);
static_assert(offsetof(terrain::PatchVertex, normal) == 12);
static_assert(offsetof(terrain::PatchVertex, biome) == 24);

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 bend_up;
    glm::vec4 bend_radii;
    glm::vec4 wind;
    glm::vec4 origin;
};

static_assert(sizeof(Push) <= 128);

constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type,
                                               VkShaderStageFlags stages) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr};
}

constexpr std::array descriptors{
    binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT),
    binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)};

// Each biome's blades: colour in linear light and height, then the share grown and their width.
struct Settings {
    std::array<glm::vec4, planet::biome_count> blade;
    std::array<glm::vec4, planet::biome_count> spread;
};

struct BiomeGrass {
    float height = 0.0F; // metres; none grows no grass
    float share = 0.0F;  // of blades grown within a clump, 0 to 1
    float width = 0.02F; // metres
    float clumps = 1.0F; // of clumps grown: 1 everywhere, low for lone tufts on bare ground
};

// Blades a touch darker than the ground's mean, as standing grass shades itself.
constexpr float blade_shade = 0.8F;

constexpr VkDeviceSize patch_bytes =
    VkDeviceSize{patch_vertex_count} * sizeof(terrain::PatchVertex);

constexpr BiomeGrass grass_of(planet::Biome biome) noexcept {
    using enum planet::Biome;
    switch (biome) {
    case Tundra:
        return {.height = 0.15F, .share = 0.4F, .width = 0.012F, .clumps = 0.6F};
    case BorealForest:
        return {.height = 0.25F, .share = 0.3F, .width = 0.015F};
    case TemperateForest:
        return {.height = 0.3F, .share = 0.4F, .width = 0.015F};
    case Grassland:
        return {.height = 0.7F, .share = 1.0F, .width = 0.018F};
    case Shrubland:
        return {.height = 0.4F, .share = 0.6F, .width = 0.015F, .clumps = 0.45F};
    // Deserts grow no lawn: tall bunchgrass in lone tufts, bare sand between.
    case Desert:
        return {.height = 0.6F, .share = 0.5F, .width = 0.02F, .clumps = 0.08F};
    case Savanna:
        return {.height = 0.9F, .share = 0.9F, .width = 0.02F, .clumps = 0.9F};
    case TropicalRainforest:
        return {.height = 0.5F, .share = 0.5F, .width = 0.018F};
    case Alpine:
        return {.height = 0.12F, .share = 0.35F, .width = 0.012F, .clumps = 0.5F};
    case Wetland:
        return {.height = 0.9F, .share = 0.9F, .width = 0.02F};
    case Ocean:
    case Beach:
    case Ice:
    case Lake:
        return {};
    }
    return {};
}

Settings grass_settings(std::span<const Vec3> palette) noexcept {
    Settings settings{};
    for (int biome = 0; biome < planet::biome_count; ++biome) {
        const BiomeGrass grass = grass_of(static_cast<planet::Biome>(biome));
        settings.blade.at(static_cast<std::size_t>(biome)) =
            glm::vec4{palette[static_cast<std::size_t>(biome)] * blade_shade, grass.height};
        settings.spread.at(static_cast<std::size_t>(biome)) =
            glm::vec4{grass.share, grass.width, grass.clumps, 0.0F};
    }
    return settings;
}

} // namespace

Result<Grass> Grass::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                            std::span<const Vec3> palette) {
    if (palette.size() != planet::biome_count) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("{} colours for {} biomes", palette.size(), planet::biome_count));
    }
    Grass grass;
    const Settings settings = grass_settings(palette);
    auto settings_buffer = make_filled_buffer(device, std::as_bytes(std::span{&settings, 1}),
                                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!settings_buffer) {
        return std::unexpected(std::move(settings_buffer).error());
    }
    grass.settings_ = std::move(settings_buffer).value();

    auto pipeline = make_pipeline(device, {.spirv = shaders::grass,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .cull = VK_CULL_MODE_NONE,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = descriptors});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    grass.pipeline_ = std::move(pipeline).value();
    return grass;
}

void Grass::draw(VkCommandBuffer commands, VkExtent2D extent, const TerrainPool& pool,
                 std::span<const GrassDraw> draws, const GrassView& view) const {
    if (extent.width == 0 || extent.height == 0 || draws.empty()) {
        return;
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    Push push{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
              .bend_up = glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
              .bend_radii =
                  glm::vec4{static_cast<float>(view.bend.radius),
                            static_cast<float>(view.bend.faked), static_cast<float>(view.seconds),
                            static_cast<float>(view.camera.vertical_fov / extent.height)},
              .wind = glm::vec4{Vec3{view.wind}, 0.0F},
              .origin = {}};
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    constexpr std::uint32_t blade_vertices = 6;
    constexpr std::uint32_t cells = static_cast<std::uint32_t>(terrain::patch_cells) *
                                    static_cast<std::uint32_t>(terrain::patch_cells);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, settings_.handle());
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing grass lit by the air without it");
    push_uniform_buffer(commands, pipeline_, 2, view.air.air);
    push_uniform_buffer(commands, pipeline_, 3, view.air.lighting);
    push_sampled_image(commands, pipeline_, 4, view.air.transmittance, view.air.sampler);
    push_sampled_image(commands, pipeline_, 5, view.air.aerial, view.air.sampler);
    push_sampled_image(commands, pipeline_, 6, view.air.cloud_shadow, view.air.sampler);
    vkCmdPushConstants(commands, pipeline_.layout(), stages, 0, offsetof(Push, wind), &push);
    for (const GrassDraw& draw : draws) {
        HOG_ASSERT(draw.slot < pool.capacity(), "growing grass on a slot beyond the pool");
        HOG_ASSERT(draw.blades_per_cell >= 1 && draw.blades_per_cell <= most_blades_per_cell,
                   "blades beyond what a cell packs");
        // The patch's own vertices, from an offset aligned for any device: whole pools can
        // outgrow what a storage binding may span.
        constexpr VkDeviceSize alignment = 256;
        const VkDeviceSize begin = VkDeviceSize{draw.slot} * patch_bytes;
        const VkDeviceSize aligned = begin & ~(alignment - 1);
        const auto bias = static_cast<std::uint32_t>(begin - aligned);
        push_storage_buffer(commands, pipeline_, 1, pool.vertices(), aligned, bias + patch_bytes);
        const auto fade =
            static_cast<std::uint32_t>(std::lround(std::clamp(draw.fade_far, 1.0, furthest_grass)));
        push.wind.w = std::bit_cast<float>(draw.seed);
        push.origin =
            glm::vec4{relative_to(draw.origin, view.camera.eye),
                      std::bit_cast<float>((draw.tall_only ? 1U << 26U : 0U) | (fade << 14U) |
                                           (bias << 6U) | draw.blades_per_cell)};
        vkCmdPushConstants(commands, pipeline_.layout(), stages, offsetof(Push, wind),
                           sizeof(Push) - offsetof(Push, wind), &push.wind);
        vkCmdDraw(commands, blade_vertices, cells * draw.blades_per_cell, 0, 0);
    }
}

} // namespace hog::render
