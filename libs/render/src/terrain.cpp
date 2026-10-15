#include <hog/core/assert.hpp>
#include <hog/render/device.hpp>
#include <hog/render/terrain.hpp>
#include <hog/shaders/terrain.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <ranges>
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
                                      .offset = offsetof(PatchVertex, biome)}};
constexpr std::array palette_binding{
    VkDescriptorSetLayoutBinding{.binding = 0,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                 .descriptorCount = 1,
                                 .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                                 .pImmutableSamplers = nullptr}};

void memory_barrier(VkCommandBuffer commands, VkPipelineStageFlags2 from_stage,
                    VkAccessFlags2 from_access, VkPipelineStageFlags2 to_stage,
                    VkAccessFlags2 to_access) noexcept {
    const VkMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .pNext = nullptr,
                                   .srcStageMask = from_stage,
                                   .srcAccessMask = from_access,
                                   .dstStageMask = to_stage,
                                   .dstAccessMask = to_access};
    const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                      .pNext = nullptr,
                                      .dependencyFlags = 0,
                                      .memoryBarrierCount = 1,
                                      .pMemoryBarriers = &barrier,
                                      .bufferMemoryBarrierCount = 0,
                                      .pBufferMemoryBarriers = nullptr,
                                      .imageMemoryBarrierCount = 0,
                                      .pImageMemoryBarriers = nullptr};
    vkCmdPipelineBarrier2(commands, &dependency);
}

} // namespace

Result<TerrainPool> TerrainPool::create(const Device& device, const TerrainPoolDesc& desc) {
    if (desc.patches == 0 || desc.frames == 0 || desc.uploads == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "a terrain pool needs a patch, a frame and an upload");
    }
    TerrainPool pool;
    auto vertices = make_buffer(
        device, patch_bytes * desc.patches,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Gpu);
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
    // Earlier frames may still be drawing from a slot about to be replaced.
    memory_barrier(commands, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_NONE,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBuffer(commands, staging_[frame].handle(), vertices_.handle(),
                    static_cast<std::uint32_t>(regions.size()), regions.data());
    memory_barrier(commands, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                   VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
    staged.clear();
    return {};
}

Result<Terrain> Terrain::create(const Device& device, VkFormat colour_format, VkFormat depth_format,
                                const BiomePalette& palette) {
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

    std::array<glm::vec4, planet::biome_count> colours{};
    std::ranges::transform(palette, colours.begin(),
                           [](Vec3 colour) { return glm::vec4{colour, 1.0F}; });
    auto palette_buffer = make_filled_buffer(device, std::as_bytes(std::span{colours}),
                                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!palette_buffer) {
        return std::unexpected(std::move(palette_buffer).error());
    }
    drawn.palette_ = std::move(palette_buffer).value();

    auto pipeline = make_pipeline(device, {.spirv = shaders::terrain,
                                           .bindings = vertex_bindings,
                                           .attributes = vertex_attributes,
                                           .colour_format = colour_format,
                                           .depth_format = depth_format,
                                           .cull = VK_CULL_MODE_BACK_BIT,
                                           .push_constant_bytes = sizeof(Push),
                                           .descriptors = palette_binding});
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
    Push push{.clip_from_eye = Mat4{view_projection(view.camera, aspect)},
              .sun = glm::vec4{Vec3{view.sun}, static_cast<float>(view.night_light)},
              .bend_up = glm::vec4{Vec3{view.bend.up}, static_cast<float>(view.bend.eye_height)},
              .bend_radii =
                  glm::vec4{static_cast<float>(view.bend.radius),
                            static_cast<float>(view.bend.faked), view.sea ? 1.0F : 0.0F, 0.0F},
              .origin = {}};
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    push_uniform_buffer(commands, pipeline_, 0, palette_.handle());
    const std::array vertices{pool.vertices()};
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, vertices.data(), &offset);
    vkCmdBindIndexBuffer(commands, indices_.handle(), 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(commands, pipeline_.layout(), stages, 0, offsetof(Push, origin), &push);
    for (const TerrainDraw& draw : draws) {
        HOG_ASSERT(draw.slot < pool.capacity(), "drawing a slot beyond the pool");
        HOG_ASSERT(draw.coarser_edges < terrain::stitchings, "stitching beyond four edges");
        push.origin = glm::vec4{relative_to(draw.origin, view.camera.eye), 0.0F};
        vkCmdPushConstants(commands, pipeline_.layout(), stages, offsetof(Push, origin),
                           sizeof(push.origin), &push.origin);
        const Range& range = stitchings_.at(draw.coarser_edges);
        vkCmdDrawIndexed(commands, range.count, 1, range.first,
                         static_cast<std::int32_t>(draw.slot * patch_vertex_count), 0);
    }
}

} // namespace hog::render
