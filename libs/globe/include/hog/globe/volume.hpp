#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/cube.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace hog::globe {

// The world's voxels, held where they are: tiles `tile_side` voxels across along a face and up
// through the layers, each a VoxelTree. Air is never held; a tile of nothing else is no tile.
// Each tile column is made down to its floor only; below that nothing is made yet, not even air.
class Volume {
public:
    static constexpr int tile_side = 64;

    // The lowest layer made in the tile column holding `site`; none where nothing is.
    [[nodiscard]] std::optional<std::uint32_t> floor(Site site) const;
    void set_floor(Site site, std::uint32_t layer);
    [[nodiscard]] bool made(Site site) const;

    // Air wherever nothing is held, made or not: ask `made` first.
    [[nodiscard]] Cube at(Site site) const;
    // False, changing nothing, where the site isn't made.
    bool set(Site site, const Cube& voxel);

    // Puts `voxel` on the voxel `below`; false where that's air or unmade, or what's over it
    // isn't air.
    bool put_on(Site below, const Cube& voxel);
    // Takes the voxel away, leaving air.
    bool remove(Site site) { return set(site, {}); }

    [[nodiscard]] std::size_t tiles() const noexcept { return tiles_.size(); }
    // The tile holding `site`; none where it's all air.
    [[nodiscard]] const VoxelTree* tile_at(Site site) const;
    // Puts the tile holding `site`, none where it's all air.
    void put_tile(Site site, VoxelTree tile);
    // Takes in the other's tiles and floors, over any of its own in the same places.
    void merge(Volume other);

    // Each floor, then each tile, in order: tiles as their place and their tree's bytes.
    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't a volume.
    [[nodiscard]] static Result<Volume> decode(std::span<const std::byte> bytes);

    // The tile column holding `site`, its floor and tiles, as bytes; empty where it isn't made.
    [[nodiscard]] std::vector<std::byte> encode_column(Site site) const;
    // Puts back the tile column holding `site` from its bytes, in place of what was there; fails,
    // changing nothing, on bytes that aren't one.
    [[nodiscard]] Result<void> decode_column(Site site, std::span<const std::byte> bytes);

    [[nodiscard]] bool same_as(const Volume& other) const;

private:
    struct Key {
        std::uint8_t face = 0;
        std::uint32_t i = 0;
        std::uint32_t j = 0;
        std::uint32_t k = 0;
        friend constexpr auto operator<=>(const Key&, const Key&) noexcept = default;
    };
    struct Column {
        std::uint8_t face = 0;
        std::uint32_t i = 0;
        std::uint32_t j = 0;
        friend constexpr auto operator<=>(const Column&, const Column&) noexcept = default;
    };
    [[nodiscard]] static Key key_of(Site site) noexcept;
    [[nodiscard]] static Column column_of(Site site) noexcept;

    std::map<Column, std::uint32_t> floors_;
    std::map<Key, VoxelTree> tiles_;
};

} // namespace hog::globe
