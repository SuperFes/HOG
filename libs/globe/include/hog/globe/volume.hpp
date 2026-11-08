#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cube.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_tree.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace hog::globe {

// Where a tile stands: its level, at which each of its voxels stands for 2^level of the finest a
// side, its face, and its place counted in tiles of that level.
struct TileKey {
    std::uint8_t level = 0;
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    std::uint32_t k = 0;

    // The tile a level coarser holding this one, and which of its eight this is: bits 1 across,
    // 2 along and 4 up.
    [[nodiscard]] constexpr TileKey parent() const noexcept {
        return {.level = static_cast<std::uint8_t>(level + 1),
                .face = face,
                .i = i / 2,
                .j = j / 2,
                .k = k / 2};
    }
    [[nodiscard]] constexpr std::size_t octant() const noexcept {
        return (i % 2) + (2 * (j % 2)) + (4 * (k % 2));
    }
    // Precondition: a level coarser than the finest.
    [[nodiscard]] constexpr TileKey child(std::size_t octant) const noexcept {
        return {.level = static_cast<std::uint8_t>(level - 1),
                .face = face,
                .i = (2 * i) + static_cast<std::uint32_t>(octant & 1U),
                .j = (2 * j) + static_cast<std::uint32_t>((octant >> 1U) & 1U),
                .k = (2 * k) + static_cast<std::uint32_t>((octant >> 2U) & 1U)};
    }

    friend constexpr auto operator<=>(const TileKey&, const TileKey&) noexcept = default;
};

// A tile column's floor and tiles as they're stored and sent. Halved, its tiles are each half as
// wide and its floor counted in voxels twice as tall.
struct TileColumn {
    std::uint32_t floor = 0;
    std::vector<std::pair<std::uint32_t, VoxelTree>> tiles; // by tile up the column, bottom up

    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't one with tiles `across` voxels wide.
    [[nodiscard]] static Result<TileColumn> decode(std::span<const std::byte> bytes, int across);
    // Precondition: its tiles are wider than one voxel.
    [[nodiscard]] TileColumn halved() const;
};

// A brick at the foot of each column of bricks in the tile column at `corner`: for whoever looks at
// the ground from above.
[[nodiscard]] std::vector<BrickSite> brick_footprint(Site corner);

// The world's voxels, held where they are: tiles `tile_side` voxels across along a face and up
// through the layers, each a VoxelTree, held from the lowest made in each tile column up to its
// highest holding anything; under that nothing is made yet, not even air, and over it is air.
// Coarser tiles over them each stand for the eight a level finer.
class Volume {
public:
    static constexpr int tile_side = 64;

    // The lowest layer made in the tile column holding `site`, the foot of its lowest tile; none
    // where nothing is.
    [[nodiscard]] std::optional<std::uint32_t> floor(Site site) const;
    // Makes the tile column holding `site` from the tile holding `layer` up, with air where it held
    // nothing.
    void set_floor(Site site, std::uint32_t layer);
    [[nodiscard]] bool made(Site site) const;
    // The layer over the highest tile held up the tile column through `site`; its floor where it
    // holds none, and 0 where it isn't made.
    [[nodiscard]] std::uint32_t held_top(Site site) const;
    // The highest layer holding matter in the column through `site`; none where it holds none.
    [[nodiscard]] std::optional<std::uint32_t> top(Site site) const;

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
    // The tile holding `site`; none where none is held.
    [[nodiscard]] const VoxelTree* tile_at(Site site) const;
    // Puts the tile holding `site`, making it.
    void put_tile(Site site, VoxelTree tile);

    // The tile at `key`, at any level; none where none is held.
    [[nodiscard]] const VoxelTree* tile(TileKey key) const;
    // Holds `tile` at a level coarser than the finest, in place of any there.
    void put_coarse(TileKey key, VoxelTree tile);
    // The finest tile held over `site`, at `level` or coarser, and its level; none where none is.
    [[nodiscard]] std::optional<std::pair<std::uint8_t, const VoxelTree*>>
    finest_over(Site site, std::uint8_t level = 0) const;
    // The voxel at `site` as the finest tile held over it has it, standing for those it covers;
    // air where none is.
    [[nodiscard]] Cube finest_at(Site site) const;
    // Makes the tiles over `key` again from theirs under them, up to `top`: each a level coarser
    // than what it's made from, an eighth from each it holds, and the rest kept as it was.
    void refresh_over(TileKey key, std::uint8_t top);
    // Takes in the other's tiles, over any of its own in the same places.
    void merge(Volume other);

    // Each tile, in order: its place and its tree's bytes.
    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't a volume.
    [[nodiscard]] static Result<Volume> decode(std::span<const std::byte> bytes);

    // The tile column holding `site`; none where it isn't made.
    [[nodiscard]] std::optional<TileColumn> column(Site site) const;
    // The same as bytes; empty where it isn't made.
    [[nodiscard]] std::vector<std::byte> encode_column(Site site) const;
    // Puts back the tile column holding `site` from its bytes, in place of what was there; fails,
    // changing nothing, on bytes that aren't one.
    [[nodiscard]] Result<void> decode_column(Site site, std::span<const std::byte> bytes);
    // Lets the tile column holding `site` go, its tiles, as if never made; what of it
    // is unsaved is lost.
    void drop_column(Site site);
    // The corners of the tile columns made, in order.
    [[nodiscard]] std::vector<Site> columns() const;

    [[nodiscard]] bool same_as(const Volume& other) const;

    // Each voxel set to something else counts a revision: the bricks changed since one, in order,
    // each once.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::vector<BrickSite> changed_since(std::uint64_t revision) const;

    // The tile columns changed since they were last saved, as their corners, in order: set, given
    // made or given a tile. Those decoded or merged in are as saved.
    [[nodiscard]] std::vector<Site> unsaved() const;
    void saved(Site site) { unsaved_.erase(column_of(site)); }

private:
    using Key = TileKey;
    struct Column {
        std::uint8_t face = 0;
        std::uint32_t i = 0;
        std::uint32_t j = 0;
        friend constexpr auto operator<=>(const Column&, const Column&) noexcept = default;
    };
    [[nodiscard]] static Key key_of(Site site) noexcept;
    [[nodiscard]] static Column column_of(Site site) noexcept;

    std::map<Key, VoxelTree> tiles_;
    std::uint8_t coarsest_ = 0; // the coarsest level any tile is held at
    std::uint64_t revision_ = 0;
    std::vector<std::pair<std::uint64_t, BrickSite>> log_; // revision and brick, in order
    std::set<Column> unsaved_;
};

} // namespace hog::globe
