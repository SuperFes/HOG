#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/world.hpp>

#include <cstdint>

namespace hog::globe {

// A square of tile columns on one of the planet's six sides, counted in tiles.
struct Area {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    std::uint32_t across = 1;

    friend bool operator==(const Area&, const Area&) = default;
};

// Tile columns along a side of one of the planet's six sides.
[[nodiscard]] std::uint32_t tiles_across(const Generator& generator);
// `across` tile columns square about the one under `direction`, kept on its side.
[[nodiscard]] Area around(const Generator& generator, DVec3 direction, std::uint32_t across);

// How far under its lowest ground a tile column is made ahead, in layers.
inline constexpr int made_depth = 16;

// The tiles up the tile column at (`face`, `i`, `j`), counted in tiles, as the generator makes
// them: from the tile `made_depth` under its lowest ground is in, its floor, to the highest ground
// or sea over it. Reads nothing else, so tile columns can be made side by side and merged.
[[nodiscard]] Volume generate_tiles(const Generator& generator, std::uint8_t face, std::uint32_t i,
                                    std::uint32_t j);

// The layers a level coarser `level` times: each voxel 2^level of the finest a side.
[[nodiscard]] Layers coarser_layers(const Layers& layers, int level) noexcept;

// The tile at `key` as the generator makes it at the tile's own level, each voxel sampled at its
// size: the same maths at any level, nothing finer made for it. Reads nothing else.
[[nodiscard]] VoxelTree generate_tile(const Generator& generator, TileKey key);

// Every tile column of the area, as `generate_tiles` makes them, one after another.
[[nodiscard]] Volume generate_area(const Generator& generator, const Area& area);

// A world over the generator's tile columns, made in memory as the world build makes them, about
// the places asked for: for tools and tests, never a running world.
class MadeWorld {
public:
    explicit MadeWorld(const Generator& generator) : world_{generator, volume_} {}
    MadeWorld(const MadeWorld&) = delete;
    MadeWorld& operator=(const MadeWorld&) = delete;
    MadeWorld(MadeWorld&&) = delete;
    MadeWorld& operator=(MadeWorld&&) = delete;
    ~MadeWorld() = default;

    // Makes the area's tile columns not made yet.
    void cover(const Area& area);
    // Makes the tile columns `across` square about the one under `direction` not made yet.
    void cover(DVec3 direction, std::uint32_t across = 3);
    // The same, made down to `layer` at least, as digging toward it would.
    void cover_down(DVec3 direction, int layer, std::uint32_t across = 3);

    [[nodiscard]] World& world() noexcept { return world_; }
    [[nodiscard]] Volume& volume() noexcept { return volume_; }

private:
    Volume volume_;
    World world_;
};

// Makes the tile under the tile column's lowest, lowering its floor by a tile; false where the
// column isn't made or is made to the bottom.
bool deepen(Volume& volume, const Generator& generator, std::uint8_t face, std::uint32_t i,
            std::uint32_t j);

} // namespace hog::globe
