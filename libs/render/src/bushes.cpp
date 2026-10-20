
#include <hog/core/assert.hpp>
#include <hog/flora/stand.hpp>
#include <hog/planet/climate.hpp>
#include <hog/render/bushes.hpp>
#include <hog/render/device.hpp>
#include <hog/shaders/bushes.hpp>

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
static_assert(sizeof(terrain::PatchVertex) == 36);
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

// Each biome's bushes: leaf colour and height, how many lattice cells grow one and the mix of
// kinds, then the tree stands they keep to the edges of.
struct Settings {
    std::array<glm::vec4, planet::biome_count> leaf;
    std::array<glm::vec4, planet::biome_count> shapes;
    std::array<glm::vec4, planet::biome_count> woods;
};

struct BiomeBushes {
    float height = 0.0F; // metres; none grows no bushes
    float share = 0.0F;  // of lattice cells grown, 0 to 1
    float floofy = 0.0F; // kinds' shares; low mats take what's left
    float spiky = 0.0F;
    float twiggy = 0.0F;
};

constexpr VkDeviceSize patch_bytes =
    VkDeviceSize{patch_vertex_count} * sizeof(terrain::PatchVertex);

constexpr BiomeBushes bushes_of(planet::Biome biome) noexcept {
    using enum planet::Biome;
    switch (biome) {
    case Tundra:
        return {.height = 0.35F, .share = 0.3F, .floofy = 0.0F, .spiky = 0.0F, .twiggy = 0.3F};
    case BorealForest:
        return {.height = 0.7F, .share = 0.25F, .floofy = 0.4F, .spiky = 0.0F, .twiggy = 0.3F};
    case TemperateForest:
        return {.height = 0.9F, .share = 0.3F, .floofy = 0.5F, .spiky = 0.0F, .twiggy = 0.3F};
    case Grassland:
        return {.height = 0.9F, .share = 0.07F, .floofy = 0.6F, .spiky = 0.0F, .twiggy = 0.3F};
    case Shrubland:
        return {.height = 1.1F, .share = 0.55F, .floofy = 0.4F, .spiky = 0.15F, .twiggy = 0.25F};
    case Desert:
        return {.height = 0.8F, .share = 0.08F, .floofy = 0.0F, .spiky = 0.6F, .twiggy = 0.3F};
    case Savanna:
        return {.height = 1.2F, .share = 0.18F, .floofy = 0.5F, .spiky = 0.2F, .twiggy = 0.3F};
    case TropicalRainforest:
        return {.height = 1.3F, .share = 0.4F, .floofy = 0.7F, .spiky = 0.1F, .twiggy = 0.0F};
    case Alpine:
        return {.height = 0.25F, .share = 0.15F, .floofy = 0.0F, .spiky = 0.0F, .twiggy = 0.2F};
    case Wetland:
        return {.height = 0.8F, .share = 0.12F, .floofy = 0.5F, .spiky = 0.0F, .twiggy = 0.5F};
    case Ocean:
    case Beach:
    case Ice:
    case Lake:
        return {};
    }
    return {};
}

// Leaves a touch darker and greener than the ground's mean, so bushes sit in their biome.
constexpr Vec3 leaf_colour(Vec3 ground) noexcept {
    return (ground * 0.55F) + (Vec3{0.02F, 0.065F, 0.02F});
}

Settings bush_settings(std::span<const Vec3> palette) noexcept {
    Settings settings{};
    for (int biome = 0; biome < planet::biome_count; ++biome) {
        const BiomeBushes bushes = bushes_of(static_cast<planet::Biome>(biome));
        settings.leaf.at(static_cast<std::size_t>(biome)) =
            glm::vec4{leaf_colour(palette[static_cast<std::size_t>(biome)]), bushes.height};
        settings.shapes.at(static_cast<std::size_t>(biome)) =
            glm::vec4{bushes.share, bushes.floofy, bushes.spiky, bushes.twiggy};
        const flora::BiomeTrees trees = flora::trees_of(static_cast<planet::Biome>(biome));
        settings.woods.at(static_cast<std::size_t>(biome)) =
            glm::vec4{trees.edge, trees.inside > 0.0F ? 1.0F : 0.0F, 0.0F, 0.0F};
    }
    return settings;
}

} // namespace

Result<Bushes> Bushes::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                              std::span<const Vec3> palette) {
    if (palette.size() != planet::biome_count) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("{} colours for {} biomes", palette.size(), planet::biome_count));
    }
    Bushes bushes;
    const Settings settings = bush_settings(palette);
    auto settings_buffer = make_filled_buffer(device, std::as_bytes(std::span{&settings, 1}),
                                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!settings_buffer) {
        return std::unexpected(std::move(settings_buffer).error());
    }
    bushes.settings_ = std::move(settings_buffer).value();

    auto pipeline = make_pipeline(device, {.spirv = shaders::bushes,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .cull = VK_CULL_MODE_NONE,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = descriptors});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    bushes.pipeline_ = std::move(pipeline).value();
    return bushes;
}

void Bushes::draw(VkCommandBuffer commands, VkExtent2D extent, const TerrainPool& pool,
                  std::span<const BushDraw> draws, const GrassView& view) const {
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
    constexpr std::uint32_t bush_vertices = 18; // three crossed quads

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, settings_.handle());
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing bushes lit by the air without it");
    push_uniform_buffer(commands, pipeline_, 2, view.air.air);
    push_uniform_buffer(commands, pipeline_, 3, view.air.lighting);
    push_sampled_image(commands, pipeline_, 4, view.air.transmittance, view.air.sampler);
    push_sampled_image(commands, pipeline_, 5, view.air.aerial, view.air.sampler);
    push_sampled_image(commands, pipeline_, 6, view.air.cloud_shadow, view.air.sampler);
    vkCmdPushConstants(commands, pipeline_.layout(), stages, 0, offsetof(Push, wind), &push);
    for (const BushDraw& draw : draws) {
        HOG_ASSERT(draw.slot < pool.capacity(), "growing bushes on a slot beyond the pool");
        HOG_ASSERT(draw.shift >= -1 && draw.shift <= 5, "a lattice shift beyond its packing");
        HOG_ASSERT(draw.lattice_i < (1U << 16U) && draw.lattice_j < (1U << 16U),
                   "a lattice base beyond its packing");
        // The patch's own vertices, from an offset aligned for any device: whole pools can
        // outgrow what a storage binding may span.
        constexpr VkDeviceSize alignment = 256;
        const VkDeviceSize begin = VkDeviceSize{draw.slot} * patch_bytes;
        const VkDeviceSize aligned = begin & ~(alignment - 1);
        const auto bias = static_cast<std::uint32_t>(begin - aligned);
        push_storage_buffer(commands, pipeline_, 1, pool.vertices(), aligned, bias + patch_bytes);
        const auto fade =
            static_cast<std::uint32_t>(std::lround(std::clamp(draw.fade_far, 1.0, furthest_grass)));
        push.wind.w = std::bit_cast<float>((draw.lattice_i << 16U) | draw.lattice_j);
        push.origin =
            glm::vec4{relative_to(draw.origin, view.camera.eye),
                      std::bit_cast<float>((fade << 14U) |
                                           (static_cast<std::uint32_t>(draw.shift + 1) << 11U) |
                                           (draw.face << 8U) | bias)};
        vkCmdPushConstants(commands, pipeline_.layout(), stages, offsetof(Push, wind),
                           sizeof(Push) - offsetof(Push, wind), &push.wind);
        const std::uint32_t side = draw.shift >= 0
                                       ? (32U >> static_cast<std::uint32_t>(draw.shift))
                                       : (32U << static_cast<std::uint32_t>(-draw.shift));
        vkCmdDraw(commands, bush_vertices, side * side, 0, 0);
    }
}

} // namespace hog::render
