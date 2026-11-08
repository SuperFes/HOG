#pragma once

#include <hog/globe/cube.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace hog::globe {

// A tile held, and where.
struct HeldTile {
    TileKey key;
    const VoxelTree* tree = nullptr;
};

// The tiles a client holds as they're sent, each at its own level and shared with whatever else
// reads them. The tiles sent cover the layers between them, so a voxel is as the finest tile held
// over it has it. A tile sent stands in place of every finer one inside it, which are let go.
class HeldTiles {
public:
    using Tree = std::shared_ptr<const VoxelTree>;

    // Precondition: `tree` is a tile, Volume::tile_side across.
    void put(TileKey key, Tree tree);

    [[nodiscard]] std::size_t size() const noexcept { return tiles_.size(); }
    // The tile at `key`, at any level; none where none is held.
    [[nodiscard]] const VoxelTree* tile(TileKey key) const;
    // The finest tile held over `key`, itself or one coarser; none where none is.
    [[nodiscard]] std::optional<HeldTile> finest_over(TileKey key) const;
    // The voxel at `site` as the finest tile held over it has it, standing for those it covers;
    // air where none is.
    [[nodiscard]] Cube at(Site site) const;
    // The highest layer under `below` holding matter in the column through `site`, the top of
    // the voxel holding it where held coarser; none where it holds none.
    [[nodiscard]] std::optional<std::uint32_t> top(Site site, std::uint32_t below) const;

private:
    std::map<TileKey, Tree> tiles_;
    std::uint8_t coarsest_ = 0; // the coarsest level any tile is held at
};

} // namespace hog::globe
