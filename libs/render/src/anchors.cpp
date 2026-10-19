#include <hog/core/assert.hpp>
#include <hog/render/anchors.hpp>
#include <hog/render/device.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <type_traits>
#include <utility>

namespace hog::render {

namespace {

// Uploaded as built, and read by the shaders at this layout.
static_assert(std::is_trivially_copyable_v<TreeAnchor> && std::is_standard_layout_v<TreeAnchor>);
static_assert(sizeof(TreeAnchor) == 40);
static_assert(offsetof(TreeAnchor, height) == 12);
static_assert(offsetof(TreeAnchor, normal) == 16);
static_assert(offsetof(TreeAnchor, hash) == 28);
static_assert(offsetof(TreeAnchor, colour) == 32);
static_assert(offsetof(TreeAnchor, age) == 36);

std::uint32_t byte_of(float value) noexcept {
    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

float from_byte(std::uint32_t packed, std::uint32_t shift) noexcept {
    return static_cast<float>((packed >> shift) & 0xffU) / 255.0F;
}

constexpr VkDeviceSize tile_bytes = VkDeviceSize{tile_trees} * sizeof(TreeAnchor);

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

std::uint32_t packed_foliage(Vec3 colour, flora::Genus genus) noexcept {
    return byte_of(std::sqrt(std::max(colour.r, 0.0F))) |
           (byte_of(std::sqrt(std::max(colour.g, 0.0F))) << 8U) |
           (byte_of(std::sqrt(std::max(colour.b, 0.0F))) << 16U) |
           (static_cast<std::uint32_t>(genus) << 24U);
}

Vec3 foliage_of(std::uint32_t packed) noexcept {
    const Vec3 rooted{from_byte(packed, 0U), from_byte(packed, 8U), from_byte(packed, 16U)};
    return rooted * rooted;
}

flora::Genus genus_of(std::uint32_t packed) noexcept {
    const std::uint32_t genus = packed >> 24U;
    return genus < static_cast<std::uint32_t>(flora::genus_count) ? static_cast<flora::Genus>(genus)
                                                                  : flora::Genus::Pine;
}

Result<AnchorPool> AnchorPool::create(const Device& device, const AnchorPoolDesc& desc) {
    if (desc.tiles == 0 || desc.frames == 0 || desc.uploads == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "an anchor pool needs a tile, a frame and an upload");
    }
    AnchorPool pool;
    auto anchors = make_buffer(
        device, tile_bytes * desc.tiles,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Memory::Gpu);
    if (!anchors) {
        return std::unexpected(std::move(anchors).error());
    }
    pool.anchors_ = std::move(anchors).value();
    for (std::uint32_t frame = 0; frame < desc.frames; ++frame) {
        auto staging = make_buffer(device, tile_bytes * desc.uploads,
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload);
        if (!staging) {
            return std::unexpected(std::move(staging).error());
        }
        pool.staging_.push_back(std::move(staging).value());
    }
    pool.staged_.resize(desc.frames);
    pool.uploads_ = desc.uploads;
    pool.held_.resize(desc.tiles);
    // Handed out from the lowest.
    pool.free_.resize(desc.tiles);
    std::ranges::copy(std::views::iota(0U, desc.tiles) | std::views::reverse, pool.free_.begin());
    return pool;
}

std::optional<std::uint32_t> AnchorPool::acquire() {
    if (free_.empty()) {
        return std::nullopt;
    }
    const std::uint32_t slot = free_.back();
    free_.pop_back();
    held_[slot] = true;
    return slot;
}

void AnchorPool::release(std::uint32_t slot) {
    HOG_ASSERT(slot < held_.size() && held_[slot], "releasing a slot that isn't held");
    held_[slot] = false;
    free_.push_back(slot);
}

Result<> AnchorPool::stage(std::uint32_t frame, std::uint32_t slot,
                           std::span<const TreeAnchor> anchors) {
    if (frame >= staging_.size()) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("no frame {} of {}", frame, staging_.size()));
    }
    if (slot >= held_.size() || !held_[slot]) {
        return make_error(ErrorCode::InvalidArgument, std::format("slot {} isn't held", slot));
    }
    if (anchors.size() > tile_trees) {
        return make_error(ErrorCode::InvalidArgument,
                          std::format("{} anchors for a tile of {}", anchors.size(), tile_trees));
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
    const auto count = static_cast<std::uint32_t>(anchors.size());
    std::ranges::copy(std::as_bytes(anchors),
                      staging_[frame].mapped().subspan(at * tile_bytes, tile_bytes).begin());
    if (again == staged.end()) {
        staged.push_back({.slot = slot, .at = at, .count = count});
    } else {
        again->count = count;
    }
    return {};
}

Result<> AnchorPool::record_uploads(VkCommandBuffer commands, std::uint32_t frame) {
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
        if (upload.count == 0) {
            continue;
        }
        regions.push_back({.srcOffset = upload.at * tile_bytes,
                           .dstOffset = upload.slot * tile_bytes,
                           .size = VkDeviceSize{upload.count} * sizeof(TreeAnchor)});
    }
    staged.clear();
    if (regions.empty()) {
        return {};
    }
    // Earlier frames may still be drawing trees from a slot about to be replaced.
    memory_barrier(commands, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_NONE,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBuffer(commands, staging_[frame].handle(), anchors_.handle(),
                    static_cast<std::uint32_t>(regions.size()), regions.data());
    memory_barrier(commands, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    return {};
}

} // namespace hog::render
