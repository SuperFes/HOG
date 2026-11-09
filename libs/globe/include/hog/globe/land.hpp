#pragma once

#include <hog/globe/brick.hpp>
#include <hog/globe/cube.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel_tree.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <span>

namespace hog::globe {

// Runs `body` for each of `count` things, in any order and on any thread: how work is spread.
using EachOf = std::function<void(std::size_t count, const std::function<void(std::size_t)>& body)>;

// The planet's ground and its waters as one octree, a tree from each top-level tile down to the
// finest voxels. Ground nobody has asked for is not made; asked for at some level, it's made by
// the generator at that level's voxels and stays open, finer to come, until asked for finer. Every
// cube of the tree stands for what's in it, so any level's tiles are windows onto the one tree.
class Land {
public:
    explicit Land(const Generator& generator);

    [[nodiscard]] const Generator& generator() const noexcept { return *generator_; }
    // The coarsest level, at which a tile spans a whole side of the planet.
    [[nodiscard]] std::uint8_t top() const noexcept { return top_; }

    // Whether the tile at `key` is known at its level all through, nothing in it known coarser.
    [[nodiscard]] bool known(TileKey key) const;
    // Makes those of the tiles not known, each by the generator at its own level, the generating
    // spread by `each` where given.
    void make(std::span<const TileKey> keys, const EachOf& each = {});
    // The tile at `key`, made first where not known: each voxel standing for its cube of the tree.
    [[nodiscard]] VoxelTree tile(TileKey key);
    // The tile at `key` as known, without making anything: safe beside other readers.
    // Precondition: known(key).
    [[nodiscard]] VoxelTree made(TileKey key) const;
    // Whether the tile at `key` is all one voxel, made first where not known.
    [[nodiscard]] bool all_one(TileKey key);
    // Whether nothing of the tile at `key` shows from outside it at any level, made first where
    // not known: as the generator made it and unchanged since, it's buried, or all one voxel with
    // no surface near it.
    [[nodiscard]] bool hidden(TileKey key);

    // The voxel at `site` as finely as it's known; air where nothing is.
    [[nodiscard]] Cube at(Site site) const;
    // Sets the voxel at `site`, its finest tile made first; every cube over it then stands for it.
    void set(Site site, const Cube& voxel);

private:
    // The top-level tile holding `key`, and where `key`'s cube stands in it.
    struct Place {
        TileKey root;
        int x = 0;
        int y = 0;
        int z = 0;
        int across = 0;
        int voxel = 0;
    };
    [[nodiscard]] Place place_of(TileKey key) const;
    [[nodiscard]] const VoxelTree* root(TileKey key) const;
    VoxelTree& root_for(TileKey key);
    void put(TileKey key, const VoxelTree& tile);

    const Generator* generator_;
    std::uint8_t top_ = 0;
    std::map<TileKey, VoxelTree> roots_;
    std::set<TileKey> hidden_;
};

// A tile's open leaves' mark as the generator makes it at `level`: 0 at the finest, as fine as it
// goes, else 1 + the level.
[[nodiscard]] constexpr std::uint8_t made_at(std::uint8_t level) noexcept {
    return level == 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(level + 1);
}

} // namespace hog::globe
