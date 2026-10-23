#include <hog/core/assert.hpp>
#include <hog/flora/stand.hpp>
#include <hog/render/anchors.hpp>
#include <hog/render/barrier.hpp>
#include <hog/render/device.hpp>
#include <hog/render/grass.hpp>
#include <hog/render/streamed_image.hpp>
#include <hog/render/terrain.hpp>
#include <hog/shaders/terrain.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace hog::render {

namespace {

using terrain::PatchVertex;

// Uploaded as baked.
static_assert(std::is_trivially_copyable_v<PatchVertex> && std::is_standard_layout_v<PatchVertex>);

constexpr VkDeviceSize patch_bytes = VkDeviceSize{patch_vertex_count} * sizeof(PatchVertex);

struct Push {
    Mat4 clip_from_eye;
    glm::vec4 sun;
    glm::vec4 bend_up;
    glm::vec4 bend_radii;
    glm::vec4 origin;
};

// Within what every device can push.
static_assert(sizeof(Push) <= 128);

constexpr std::array vertex_bindings{VkVertexInputBindingDescription{
    .binding = 0, .stride = sizeof(PatchVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}};
constexpr std::array vertex_attributes{
    VkVertexInputAttributeDescription{.location = 0,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32B32_SFLOAT,
                                      .offset = offsetof(PatchVertex, position)},
    VkVertexInputAttributeDescription{.location = 1,
                                      .binding = 0,
                                      .format = VK_FORMAT_R32G32B32_SFLOAT,
                                      .offset = offsetof(PatchVertex, normal)},
    VkVertexInputAttributeDescription{.location = 2,
                                      .binding = 0,
                                      .format = VK_FORMAT_R8_UINT,
                                      .offset = offsetof(PatchVertex, biome)},
    VkVertexInputAttributeDescription{.location = 3,
                                      .binding = 0,
                                      .format = VK_FORMAT_R8G8B8A8_UNORM,
                                      .offset = offsetof(PatchVertex, woods)},
    VkVertexInputAttributeDescription{.location = 4,
                                      .binding = 0,
                                      .format = VK_FORMAT_R8_UNORM,
                                      .offset = offsetof(PatchVertex, woods) + 4},
    VkVertexInputAttributeDescription{.location = 5,
                                      .binding = 0,
                                      .format = VK_FORMAT_R8_UNORM,
                                      .offset = offsetof(PatchVertex, cover)},
    VkVertexInputAttributeDescription{.location = 6,
                                      .binding = 0,
                                      .format = VK_FORMAT_R8_UINT,
                                      .offset = offsetof(PatchVertex, blend_biome)},
    VkVertexInputAttributeDescription{.location = 7,
                                      .binding = 0,
                                      .format = VK_FORMAT_R8_UNORM,
                                      .offset = offsetof(PatchVertex, blend)}};
constexpr VkDescriptorSetLayoutBinding binding(std::uint32_t at, VkDescriptorType type,
                                               VkShaderStageFlags stages) noexcept {
    return {.binding = at,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = stages,
            .pImmutableSamplers = nullptr};
}

// The sea, as high as it stands over the ground, and the foam it strands there.
constexpr std::uint32_t sea_binding = 7;
constexpr std::uint32_t shore_binding = 8;
constexpr std::uint32_t foam_binding = 10;

constexpr std::array descriptors{
    binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(sea_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(shore_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(foam_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)};
// Lit through the air, it reads the air too.
constexpr std::array air_descriptors{
    descriptors[0],
    descriptors[1],
    binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    descriptors[2],
    descriptors[3],
    binding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT),
    descriptors[4]};

// The near tile's size, the far one's, and 1 if ground below sea level is seen through water; then
// what each biome's material averages to, in linear light, and how much of it is grass waving in
// the wind. Then the woods, as terrain.slang reads them, and each biome's grass, as GrassStand.
struct Settings {
    glm::vec4 tiling;
    std::array<glm::vec4, planet::biome_count> means;
    glm::vec4 canopy;
    std::array<glm::vec4, flora::genus_count> foliage;
    GenusSeasons seasons;
    std::array<glm::vec4, planet::biome_count> grass;
};

// The far tile, as many times the near one's size.
constexpr double far_tiles = 8.0;

// Each mip of every material in turn, the biomes' layers one after another.
Result<std::vector<std::byte>> material_texels(std::span<const terrain::MaterialImage> materials) {
    if (materials.size() != planet::biome_count) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("{} materials for {} biomes", materials.size(), planet::biome_count));
    }
    const terrain::MaterialImage& first = materials.front();
    for (const terrain::MaterialImage& material : materials) {
        if (material.size != first.size || material.levels.size() != first.levels.size() ||
            first.levels.empty()) {
            return make_error(ErrorCode::InvalidArgument, "materials of different sizes");
        }
    }
    std::vector<std::byte> texels;
    for (std::size_t level = 0; level < first.levels.size(); ++level) {
        for (const terrain::MaterialImage& material : materials) {
            const auto bytes = std::as_bytes(std::span{material.levels[level]});
            texels.insert(texels.end(), bytes.begin(), bytes.end());
        }
    }
    return texels;
}

} // namespace

Result<TerrainPool> TerrainPool::create(const Device& device, const TerrainPoolDesc& desc) {
    if (desc.patches == 0 || desc.frames == 0 || desc.uploads == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "a terrain pool needs a patch, a frame and an upload");
    }
    TerrainPool pool;
    // Storage too: grass reads the patches' ground straight from the pool.
    auto vertices =
        make_buffer(device, patch_bytes * desc.patches,
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    Memory::Gpu);
    if (!vertices) {
        return std::unexpected(std::move(vertices).error());
    }
    pool.vertices_ = std::move(vertices).value();
    for (std::uint32_t frame = 0; frame < desc.frames; ++frame) {
        auto staging = make_buffer(device, patch_bytes * desc.uploads,
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload);
        if (!staging) {
            return std::unexpected(std::move(staging).error());
        }
        pool.staging_.push_back(std::move(staging).value());
    }
    pool.staged_.resize(desc.frames);
    pool.uploads_ = desc.uploads;
    pool.held_.resize(desc.patches);
    // Handed out from the lowest.
    pool.free_.resize(desc.patches);
    std::ranges::copy(std::views::iota(0U, desc.patches) | std::views::reverse, pool.free_.begin());
    return pool;
}

std::optional<std::uint32_t> TerrainPool::acquire() {
    if (free_.empty()) {
        return std::nullopt;
    }
    const std::uint32_t slot = free_.back();
    free_.pop_back();
    held_[slot] = true;
    return slot;
}

void TerrainPool::release(std::uint32_t slot) {
    HOG_ASSERT(slot < held_.size() && held_[slot], "releasing a slot that isn't held");
    held_[slot] = false;
    free_.push_back(slot);
}

Result<> TerrainPool::stage(std::uint32_t frame, std::uint32_t slot,
                            std::span<const terrain::PatchVertex> vertices) {
    if (frame >= staging_.size()) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("no frame {} of {}", frame, staging_.size()));
    }
    if (slot >= held_.size() || !held_[slot]) {
        return make_error(ErrorCode::InvalidArgument, std::format("slot {} isn't held", slot));
    }
    if (vertices.size() != patch_vertex_count) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("{} vertices for a patch of {}", vertices.size(), patch_vertex_count));
    }
    std::vector<Staged>& staged = staged_[frame];
    // Staging a slot again in the same frame replaces what was staged.
    const auto again = std::ranges::find(staged, slot, &Staged::slot);
    if (again == staged.end() && staged.size() >= uploads_) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("more than {} uploads in a frame", uploads_));
    }
    const std::uint32_t at =
        again == staged.end() ? static_cast<std::uint32_t>(staged.size()) : again->at;
    std::ranges::copy(std::as_bytes(vertices),
                      staging_[frame].mapped().subspan(at * patch_bytes, patch_bytes).begin());
    if (again == staged.end()) {
        staged.push_back({.slot = slot, .at = at});
    }
    return {};
}

Result<> TerrainPool::record_uploads(VkCommandBuffer commands, std::uint32_t frame) {
    if (frame >= staging_.size()) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("no frame {} of {}", frame, staging_.size()));
    }
    std::vector<Staged>& staged = staged_[frame];
    if (staged.empty()) {
        return {};
    }
    if (auto flushed = staging_[frame].flush(); !flushed) {
        return flushed;
    }
    std::vector<VkBufferCopy> regions;
    regions.reserve(staged.size());
    for (const Staged& upload : staged) {
        regions.push_back({.srcOffset = upload.at * patch_bytes,
                           .dstOffset = upload.slot * patch_bytes,
                           .size = patch_bytes});
    }
    // Earlier frames may still be drawing from a slot about to be replaced, the ground as
    // vertices and the grass through storage reads.
    memory_barrier(commands,
                   VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                       VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBuffer(commands, staging_[frame].handle(), vertices_.handle(),
                    static_cast<std::uint32_t>(regions.size()), regions.data());
    memory_barrier(commands, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                       VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                   VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    staged.clear();
    return {};
}

Result<Terrain> Terrain::create(Device& device, VkFormat colour_format, VkFormat depth_format,
                                const TerrainLook& look) {
    Terrain drawn;
    std::vector<std::uint16_t> indices;
    for (std::size_t edges = 0; edges < drawn.stitchings_.size(); ++edges) {
        const auto surface = terrain::patch_indices(static_cast<std::uint8_t>(edges));
        const auto skirts = terrain::skirt_indices(static_cast<std::uint8_t>(edges));
        drawn.stitchings_.at(edges) = {
            .first = static_cast<std::uint32_t>(indices.size()),
            .count = static_cast<std::uint32_t>(surface.size() + skirts.size())};
        indices.insert(indices.end(), surface.begin(), surface.end());
        indices.insert(indices.end(), skirts.begin(), skirts.end());
    }
    auto index_buffer = make_filled_buffer(device, std::as_bytes(std::span{indices}),
                                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!index_buffer) {
        return std::unexpected(std::move(index_buffer).error());
    }
    drawn.indices_ = std::move(index_buffer).value();

    auto texels = material_texels(look.materials);
    if (!texels) {
        return std::unexpected(std::move(texels).error());
    }
    const terrain::MaterialImage& first = look.materials.front();
    auto materials =
        make_filled_image(device,
                          {.format = VK_FORMAT_R8G8B8A8_SRGB,
                           .extent = {.width = static_cast<std::uint32_t>(first.size),
                                      .height = static_cast<std::uint32_t>(first.size)},
                           .layers = planet::biome_count,
                           .levels = static_cast<std::uint32_t>(first.levels.size())},
                          *texels);
    if (!materials) {
        return std::unexpected(std::move(materials).error());
    }
    drawn.materials_ = std::move(materials).value();
    auto sampler = make_tiling_sampler(device);
    if (!sampler) {
        return std::unexpected(std::move(sampler).error());
    }
    drawn.sampler_ = std::move(sampler).value();

    drawn.period_ = look.tile * far_tiles;
    Settings settings{.tiling = {static_cast<float>(look.tile), static_cast<float>(drawn.period_),
                                 look.sea ? 1.0F : 0.0F, 0.0F},
                      .means = {},
                      .canopy = {static_cast<float>(look.canopy),
                                 look.air ? static_cast<float>(look.grass) : 0.0F, 0.0F, 0.0F},
                      .foliage = {},
                      .seasons = genus_seasons(),
                      .grass = {}};
    for (std::size_t biome = 0; biome < settings.means.size(); ++biome) {
        settings.means.at(biome) = glm::vec4{Vec3{terrain::material_mean(look.materials[biome])},
                                             waving_grass(static_cast<planet::Biome>(biome))};
        const GrassStand stand = grass_stand(static_cast<planet::Biome>(biome));
        settings.grass.at(biome) = glm::vec4{stand.height, stand.cover, 0.0F, 0.0F};
    }
    for (std::size_t genus = 0; genus < flora::genus_count; ++genus) {
        const flora::Foliage foliage = flora::foliage_of(static_cast<flora::Genus>(genus));
        settings.foliage.at(genus) = glm::vec4{foliage.own, foliage.of_ground};
    }
    auto settings_buffer = make_filled_buffer(device, std::as_bytes(std::span{&settings, 1}),
                                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!settings_buffer) {
        return std::unexpected(std::move(settings_buffer).error());
    }
    drawn.settings_ = std::move(settings_buffer).value();

    auto calm = CalmSea::create(device);
    if (!calm) {
        return std::unexpected(std::move(calm).error());
    }
    drawn.calm_ = std::move(calm).value();

    drawn.air_ = look.air;
    auto pipeline = make_pipeline(
        device,
        {.spirv = shaders::terrain,
         .vertex_entry = look.air ? "vertex_air" : "vertex_main",
         .fragment_entry = look.air ? "fragment_air" : "fragment_main",
         .bindings = vertex_bindings,
         .attributes = vertex_attributes,
         .colour_format = colour_format,
         .depth_format = depth_format,
         .cull = VK_CULL_MODE_BACK_BIT,
         .push_constant_bytes = sizeof(Push),
         .descriptors = look.air ? std::span<const VkDescriptorSetLayoutBinding>{air_descriptors}
                                 : std::span<const VkDescriptorSetLayoutBinding>{descriptors}});
    if (!pipeline) {
        return std::unexpected(std::move(pipeline).error());
    }
    drawn.pipeline_ = std::move(pipeline).value();
    return drawn;
}

void Terrain::draw(VkCommandBuffer commands, VkExtent2D extent, const TerrainPool& pool,
                   std::span<const TerrainDraw> draws, const TerrainView& view) const {
    if (extent.width == 0 || extent.height == 0 || draws.empty()) {
        return;
    }
    const double aspect = static_cast<double>(extent.width) / static_cast<double>(extent.height);
    // Where the eye lies in the tiling, which keeps materials' coordinates small near it.
    const Vec3 tiled{glm::mod(view.camera.eye.metres(), DVec3{period_})};
    Push push{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
              .sun = glm::vec4{Vec3{view.sun}, static_cast<float>(view.night_light)},
              .bend_up = glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
              .bend_radii = glm::vec4{static_cast<float>(view.bend.radius),
                                      static_cast<float>(view.bend.faked), Vec2{tiled}},
              .origin = {}};
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, settings_.handle());
    push_sampled_image(commands, pipeline_, 1, materials_.view(), sampler_.handle());
    if (air_) {
        HOG_ASSERT(view.air.lighting != VK_NULL_HANDLE, "drawing ground lit by the air without it");
        push_uniform_buffer(commands, pipeline_, 2, view.air.air);
        push_uniform_buffer(commands, pipeline_, 3, view.air.lighting);
        push_sampled_image(commands, pipeline_, 4, view.air.transmittance, view.air.sampler);
        push_sampled_image(commands, pipeline_, 5, view.air.aerial, view.air.sampler);
        push_sampled_image(commands, pipeline_, 6, view.air.cloud_shadow, view.air.sampler);
        push_sampled_image(commands, pipeline_, 9, view.air.sky_view, view.air.sampler);
    }
    const SeaBindings sea = view.sea.waves != VK_NULL_HANDLE ? view.sea : calm_.bindings();
    push_uniform_buffer(commands, pipeline_, sea_binding, sea.waves);
    push_sampled_image(commands, pipeline_, shore_binding, sea.shore, sea.sampler);
    push_sampled_image(commands, pipeline_, foam_binding, sea.foam, sea.sampler);
    const std::array vertices{pool.vertices()};
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, vertices.data(), &offset);
    vkCmdBindIndexBuffer(commands, indices_.handle(), 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(commands, pipeline_.layout(), stages, 0, offsetof(Push, origin), &push);
    for (const TerrainDraw& draw : draws) {
        HOG_ASSERT(draw.slot < pool.capacity(), "drawing a slot beyond the pool");
        HOG_ASSERT(draw.coarser_edges < terrain::stitchings, "stitching beyond four edges");
        push.origin = glm::vec4{relative_to(draw.origin, view.camera.eye), tiled.z};
        vkCmdPushConstants(commands, pipeline_.layout(), stages, offsetof(Push, origin),
                           sizeof(push.origin), &push.origin);
        const Range& range = stitchings_.at(draw.coarser_edges);
        vkCmdDrawIndexed(commands, range.count, 1, range.first,
                         static_cast<std::int32_t>(draw.slot * patch_vertex_count), 0);
    }
}

} // namespace hog::render
