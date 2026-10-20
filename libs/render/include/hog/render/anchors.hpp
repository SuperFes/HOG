#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/flora/bushes.hpp>
#include <hog/flora/species.hpp>
#include <hog/flora/stand.hpp>
#include <hog/render/memory.hpp>
#include <hog/render/vulkan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::render {

class Device;

// A tree bound to its own spot in the world, sampled from the world itself rather than the drawn
// ground, so it stands there at every terrain level, on every machine, for ever — until the world
// changes under it. Both tree tiers draw from the same anchors, so they can never disagree.
struct TreeAnchor {
    Vec3 position{0.0F};           // metres from its tile's origin
    float height = 0.0F;           // the tree's, metres
    Vec3 normal{0.0F, 0.0F, 1.0F}; // the ground's, unit
    std::uint32_t hash = 0;        // the tree's own dice
    std::uint32_t colour = 0; // its foliage's, genus and condition, as packed_foliage() packs them
    float age = 0.0F;         // 0 a sapling, 1 full grown
};

// A bush bound to its own spot in the world, as a tree anchor is, held in the same pools.
struct BushAnchor {
    Vec3 position{0.0F};           // metres from its tile's origin
    float height = 0.0F;           // the bush's, metres
    Vec3 normal{0.0F, 0.0F, 1.0F}; // the ground's, unit
    std::uint32_t hash = 0;        // the bush's own dice
    std::uint32_t colour = 0;      // its leaves' and its kind, as packed_leaves() packs them
    float width = 0.0F;            // metres
};

// Foliage's colour in a byte a channel, kept square-rooted so dark needles keep their shades, with
// the tree's genus in the low half of the last byte and its condition in the high.
[[nodiscard]] std::uint32_t
packed_foliage(Vec3 colour, flora::Genus genus,
               flora::Condition condition = flora::Condition::Sound) noexcept;
[[nodiscard]] Vec3 foliage_of(std::uint32_t packed) noexcept;
[[nodiscard]] flora::Genus genus_of(std::uint32_t packed) noexcept;
[[nodiscard]] flora::Condition condition_of(std::uint32_t packed) noexcept;

// A bush's leaves packed as foliage is, with its kind in the last byte.
[[nodiscard]] std::uint32_t packed_leaves(Vec3 colour, flora::BushKind kind) noexcept;
[[nodiscard]] flora::BushKind kind_of(std::uint32_t packed) noexcept;

// Each genus' year as the tree shaders read it, from its species' flora::Leafing: x turn, y fall,
// z flush, w spread; then the colours its leaves turn through, first and last.
struct GenusSeasons {
    std::array<glm::vec4, flora::genus_count> leafing{};
    std::array<glm::vec4, flora::genus_count> first{};
    std::array<glm::vec4, flora::genus_count> last{};
};
[[nodiscard]] GenusSeasons genus_seasons();

// The fixed lattice trees are anchored to, coarser than the bushes': one tree at most in each
// level-14 cell, bound to its spot in the world for ever.
inline constexpr int tree_lattice_level = 14;
// Anchors a tile holds at most; a tile is the cell 16 by 16 lattice cells make, level 10 for
// trees and 12 for bushes.
inline constexpr std::uint32_t tile_trees = 256;

struct AnchorPoolDesc {
    std::uint32_t tiles = 1;   // held on the GPU at once
    std::uint32_t frames = 1;  // in flight, each staging through its own buffer
    std::uint32_t uploads = 1; // staged per frame at most
};

// Tiles' tree or bush anchors on the GPU, a slot each, staged and uploaded as the terrain pool's
// patches are.
class AnchorPool {
public:
    [[nodiscard]] static Result<AnchorPool> create(const Device& device,
                                                   const AnchorPoolDesc& desc);

    [[nodiscard]] VkBuffer anchors() const noexcept { return anchors_.handle(); }
    [[nodiscard]] std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(held_.size());
    }
    [[nodiscard]] std::uint32_t free_slots() const noexcept {
        return static_cast<std::uint32_t>(free_.size());
    }

    [[nodiscard]] std::optional<std::uint32_t> acquire();
    // Frames in flight may still draw from it: the next upload into it waits for them.
    void release(std::uint32_t slot);

    // Copies a tile's anchors in for `frame`'s next record_uploads, within its budget. The
    // frame's last submit must have finished.
    Result<> stage(std::uint32_t frame, std::uint32_t slot, std::span<const TreeAnchor> anchors);
    Result<> stage(std::uint32_t frame, std::uint32_t slot, std::span<const BushAnchor> anchors);
    // Records copying what `frame` has staged into its slots, after drawing already recorded
    // from them and before any recorded next.
    Result<> record_uploads(VkCommandBuffer commands, std::uint32_t frame);

private:
    struct Staged {
        std::uint32_t slot;
        std::uint32_t at; // in the frame's staging buffer
        std::uint32_t count;
    };

    AnchorPool() = default;

    Result<> stage_bytes(std::uint32_t frame, std::uint32_t slot, std::span<const std::byte> bytes);

    Buffer anchors_;
    std::vector<Buffer> staging_;
    std::vector<std::vector<Staged>> staged_;
    std::uint32_t uploads_ = 0;
    std::vector<bool> held_;
    std::vector<std::uint32_t> free_;
};

// A tile of anchored trees to draw: the stamps shrink in from afar, the grown forest within its
// own reach, each tree deciding by its own distance, so the handoff can never tear.
struct StandDraw {
    std::uint32_t slot = 0;  // in the anchor pool
    std::uint32_t first = 0; // the first of the tile's anchors drawn
    std::uint32_t count = 0; // anchors drawn from there
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition origin = {}; // the tile's, the anchors' positions relative to it
    int detail = 0;            // the finest build the nearest tree drawn needs
    // The grown forest's draws hold one genus each, bounding their vertex counts by its own.
    flora::Genus genus = flora::Genus::Pine;
    double distance = 0.0; // metres to the nearest tree drawn, generously
};

} // namespace hog::render
