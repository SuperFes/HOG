#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/cube.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace hog::globe {

// The world's voxels, held where they are: tiles `tile_side` voxels across along a face and up
// through the layers, each a VoxelTree. Air is never held; a tile of nothing else is no tile.
class Volume {
public:
    static constexpr int tile_side = 64;

    [[nodiscard]] Cube at(Site site) const;
    void set(Site site, const Cube& voxel);

    // Puts `voxel` on the voxel `below`; false where that's air or what's over it isn't.
    bool put_on(Site below, const Cube& voxel);
    // Takes the voxel away, leaving air.
    void remove(Site site) { set(site, {}); }

    [[nodiscard]] std::size_t tiles() const noexcept { return tiles_.size(); }
    // Puts the tile holding `site`, none where it's all air.
    void put_tile(Site site, VoxelTree tile);
    // Takes in the other's tiles, over any of its own in the same places.
    void merge(Volume&& other);

    // Each tile once, in order, as its place and its tree's bytes.
    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't a volume.
    [[nodiscard]] static Result<Volume> decode(std::span<const std::byte> bytes);

    [[nodiscard]] bool same_as(const Volume& other) const;

private:
    struct Key {
        std::uint8_t face = 0;
        std::uint32_t i = 0;
        std::uint32_t j = 0;
        std::uint32_t k = 0;
        friend constexpr auto operator<=>(const Key&, const Key&) noexcept = default;
    };
    [[nodiscard]] static Key key_of(Site site) noexcept;

    std::map<Key, VoxelTree> tiles_;
};

} // namespace hog::globe
