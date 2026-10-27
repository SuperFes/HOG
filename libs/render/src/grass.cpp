#include <hog/core/assert.hpp>
#include <hog/planet/climate.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/grass.hpp>
#include <hog/shaders/grass.hpp>
#include <hog/terrain/patch.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <format>
#include <utility>
#include <vector>

namespace hog::render {

namespace {

// The shader reads the pool's vertices by this layout.
static_assert(sizeof(terrain::PatchVertex) == 40);
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
    binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)};

constexpr std::array growing_descriptors{
    binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(11, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT),
    binding(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT)};

// Six vertices, the blades grown, then neither first vertex nor first instance.
constexpr std::array<std::uint32_t, 4> no_blades{6, 0, 0, 0};
// Threads in each of the shader's groups.
constexpr std::uint32_t group_size = 64;

// Each biome's blades: colour in linear light and height, then the share grown and their width;
// how it thins, as density, hold and far; and the lattice's level and seed.
struct Settings {
    std::array<glm::vec4, planet::biome_count> blade;
    std::array<glm::vec4, planet::biome_count> spread;
    glm::vec4 tier;
    glm::uvec4 lattice;
};

// A patch grass grows on, and how deep into it its blades are sown.
struct Sown {
    const GrassDraw* draw;
    std::uint32_t depth;
};

struct BiomeGrass {
    float height = 0.0F; // metres; none grows no grass
    float share = 0.0F;  // of blades grown within a clump, 0 to 1
    float width = 0.02F; // metres
    float clumps = 1.0F; // of the ground in clumps: 1 everywhere, low for lone tufts on bare ground
    float reeds = 0.0F;  // share of stems standing in the band at the waterline
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
        return {.height = 0.9F, .share = 0.9F, .width = 0.02F, .reeds = 0.12F};
    // Lakes grow nothing on their beds, but reeds fringe their shores.
    case Lake:
        return {.reeds = 0.15F};
    case Ocean:
    case Beach:
    case Ice:
        return {};
    }
    return {};
}

} // namespace

double grass_layers(const GrassTier& tier, double spacing, double distance,
                    double thickness) noexcept {
    const double hold = tier.hold / std::sqrt(thickness);
    const double held = hold / std::max(distance, hold);
    return tier.density * thickness * spacing * spacing * held * held;
}

int grass_depth(double layers, int to_lattice) noexcept {
    const double deep = terrain::patch_depth + std::ceil(std::log2(layers) / 2.0);
    return static_cast<int>(
        std::clamp(deep, 0.0, static_cast<double>(std::min(most_grass_depth, to_lattice))));
}

std::uint32_t grass_patch_level(int level, double layers) noexcept {
    const double eighths = std::clamp(std::ceil(4.0 * std::log2(layers)), -128.0, 127.0);
    return static_cast<std::uint32_t>(level) |
           (static_cast<std::uint32_t>(static_cast<int>(eighths) + 128) << 8U);
}

GrassStand grass_stand(planet::Biome biome) noexcept {
    const BiomeGrass grass = grass_of(biome);
    return {.height = grass.height, .cover = grass.share * grass.clumps};
}

float waving_grass(planet::Biome biome) noexcept {
    // Lawns shorter than this stand stiff, and from this tall wave freely.
    constexpr float stiff = 0.15F;
    constexpr float free = 0.7F;
    const BiomeGrass grass = grass_of(biome);
    return grass.share * grass.clumps *
           std::clamp((grass.height - stiff) / (free - stiff), 0.0F, 1.0F);
}

namespace {

Settings grass_settings(std::span<const Vec3> palette, const GrassLattice& lattice,
                        const GrassTier& tier) noexcept {
    Settings settings{};
    settings.tier = {static_cast<float>(tier.density), static_cast<float>(tier.hold),
                     static_cast<float>(tier.reach), 0.0F};
    settings.lattice = {static_cast<std::uint32_t>(lattice.level), lattice.seed, 0U, 0U};
    for (int biome = 0; biome < planet::biome_count; ++biome) {
        const BiomeGrass grass = grass_of(static_cast<planet::Biome>(biome));
        settings.blade.at(static_cast<std::size_t>(biome)) =
            glm::vec4{palette[static_cast<std::size_t>(biome)] * blade_shade, grass.height};
        settings.spread.at(static_cast<std::size_t>(biome)) =
            glm::vec4{grass.share, grass.width, clump_corners_on(grass.clumps), grass.reeds};
    }
    return settings;
}

} // namespace

Result<Grass> Grass::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                            std::span<const Vec3> palette, const GrassLattice& lattice,
                            const GrassTier& tier) {
    if (palette.size() != planet::biome_count) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("{} colours for {} biomes", palette.size(), planet::biome_count));
    }
    if (lattice.level < 0 || lattice.level > planet::max_level) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("a grass lattice at level {}", lattice.level));
    }
    Grass grass;
    grass.tier_ = tier;
    grass.lattice_ = lattice;
    const Settings settings = grass_settings(palette, lattice, tier);
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

    auto growing = make_compute_pipeline(device, {.spirv = shaders::grass,
                                                  .push_constant_bytes = sizeof(Push),
                                                  .descriptors = growing_descriptors});
    if (!growing) {
        return std::unexpected(std::move(growing).error());
    }
    grass.growing_ = std::move(growing).value();
    auto calm = CalmSea::create(device);
    if (!calm) {
        return std::unexpected(std::move(calm).error());
    }
    grass.calm_ = std::move(calm).value();
    auto blades = make_buffer(device, VkDeviceSize{grown_blade_bytes} * most_grown_blades,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              Memory::Gpu);
    if (!blades) {
        return std::unexpected(std::move(blades).error());
    }
    grass.blades_ = std::move(blades).value();
    auto grown_draw =
        make_buffer(device, sizeof(no_blades),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    Memory::Gpu);
    if (!grown_draw) {
        return std::unexpected(std::move(grown_draw).error());
    }
    grass.grown_draw_ = std::move(grown_draw).value();
    auto patches = make_buffer(
        device, sizeof(glm::uvec4) * most_grass_draws,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Gpu);
    if (!patches) {
        return std::unexpected(std::move(patches).error());
    }
    grass.patches_ = std::move(patches).value();
    return grass;
}

namespace {

Push push_for(VkExtent2D extent, const GrassView& view) {
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    return {.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
            .bend_up = glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
            .bend_radii =
                glm::vec4{static_cast<float>(view.bend.radius), static_cast<float>(view.bend.faked),
                          static_cast<float>(view.seconds),
                          static_cast<float>(view.camera.vertical_fov / extent.height)},
            .wind = glm::vec4{Vec3{view.wind}, 0.0F},
            .origin = {}};
}

} // namespace

void Grass::grow(VkCommandBuffer commands, VkExtent2D extent, const TerrainPool& pool,
                 std::span<const GrassDraw> draws, const GrassView& view) const {
    std::vector<Sown> sown;
    std::vector<glm::uvec4> patches;
    if (extent.width > 0 && extent.height > 0) {
        for (const GrassDraw& draw : draws.first(std::min(draws.size(), most_grass_draws))) {
            HOG_ASSERT(draw.slot < pool.capacity(), "growing grass on a slot beyond the pool");
            HOG_ASSERT(draw.spacing > 0.0 && draw.spacing <= coarsest_grass_cells,
                       "grass on cells coarser than a draw packs");
            HOG_ASSERT(draw.cell.level <= lattice_.level,
                       "grass on a patch finer than its lattice");
            // Sown as deep as its nearest cells grow; the shader lets each blade go by its own
            // distance.
            const double layers = grass_layers(tier_, draw.spacing, draw.nearest, thickest_grass);
            if (draw.nearest >= tier_.reach || layers <= 0.0) {
                continue;
            }
            sown.push_back({.draw = &draw,
                            .depth = static_cast<std::uint32_t>(
                                grass_depth(layers, lattice_.level - draw.cell.level))});
            patches.emplace_back(static_cast<std::uint32_t>(draw.cell.face), draw.cell.i,
                                 draw.cell.j, grass_patch_level(draw.cell.level, layers));
        }
    }
    // Not until the last frame's draw has read what's grown.
    memory_barrier(commands,
                   VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_NONE,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_NONE);
    vkCmdUpdateBuffer(commands, grown_draw_.handle(), 0, sizeof(no_blades), no_blades.data());
    if (!patches.empty()) {
        vkCmdUpdateBuffer(commands, patches_.handle(), 0, patches.size() * sizeof(glm::uvec4),
                          patches.data());
    }
    memory_barrier(commands, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (!sown.empty()) {
        Push push = push_for(extent, view);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, growing_.handle());
        push_uniform_buffer(commands, growing_, 0, settings_.handle());
        HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "growing grass in the wind without it");
        push_uniform_buffer(commands, growing_, 3, view.air.lighting);
        push_storage_buffer(commands, growing_, 7, blades_.handle());
        push_storage_buffer(commands, growing_, 8, grown_draw_.handle());
        push_storage_buffer(commands, growing_, 10, patches_.handle());
        const SeaBindings sea = view.sea.waves != VK_NULL_HANDLE ? view.sea : calm_.bindings();
        push_uniform_buffer(commands, growing_, 11, sea.waves);
        push_sampled_image(commands, growing_, 12, sea.shore, sea.sampler);
        vkCmdPushConstants(commands, growing_.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           offsetof(Push, wind), &push);
        for (std::uint32_t index = 0; index < sown.size(); ++index) {
            const GrassDraw& draw = *sown[index].draw;
            const std::uint32_t depth = sown[index].depth;
            // The patch's own vertices, from an offset aligned for any device: whole pools can
            // outgrow what a storage binding may span.
            constexpr VkDeviceSize alignment = 256;
            const VkDeviceSize begin = VkDeviceSize{draw.slot} * patch_bytes;
            const VkDeviceSize aligned = begin & ~(alignment - 1);
            const auto bias = static_cast<std::uint32_t>(begin - aligned);
            push_storage_buffer(commands, growing_, 1, pool.vertices(), aligned,
                                bias + patch_bytes);
            const auto spacing = static_cast<std::uint32_t>(std::lround(draw.spacing * 256.0));
            push.wind.w = std::bit_cast<float>(index);
            push.origin = glm::vec4{relative_to(draw.origin, view.camera.eye),
                                    std::bit_cast<float>((spacing << 14U) | (bias << 6U) | depth)};
            vkCmdPushConstants(commands, growing_.layout(), VK_SHADER_STAGE_COMPUTE_BIT,
                               offsetof(Push, wind), sizeof(Push) - offsetof(Push, wind),
                               &push.wind);
            vkCmdDispatch(commands, std::max(1U, (1U << (2U * depth)) / group_size), 1, 1);
        }
    }
    memory_barrier(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void Grass::draw(VkCommandBuffer commands, VkExtent2D extent, const GrassView& view) const {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    const Push push = push_for(extent, view);
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, settings_.handle());
    HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing grass lit by the air without it");
    push_uniform_buffer(commands, pipeline_, 2, view.air.air);
    push_uniform_buffer(commands, pipeline_, 3, view.air.lighting);
    push_sampled_image(commands, pipeline_, 4, view.air.transmittance, view.air.sampler);
    push_sampled_image(commands, pipeline_, 5, view.air.aerial, view.air.sampler);
    push_sampled_image(commands, pipeline_, 6, view.air.cloud_shadow, view.air.sampler);
    push_storage_buffer(commands, pipeline_, 9, blades_.handle());
    vkCmdPushConstants(commands, pipeline_.layout(), stages, 0, sizeof(Push), &push);
    vkCmdDrawIndirect(commands, grown_draw_.handle(), 0, 1, sizeof(no_blades));
}

} // namespace hog::render
