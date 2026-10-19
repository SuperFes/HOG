#pragma once

#include <hog/client/ground.hpp>
#include <hog/core/result.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/world.hpp>
#include <hog/render/anchors.hpp>

#include <cstdint>
#include <map>
#include <vector>

namespace hog::client {

// Tiles of anchored trees: each a level-10 cell holding its 16 by 16 lattice cells' trees,
// sampled once from the world itself — never the drawn ground — so every tree keeps its spot,
// height and lean at every terrain level, until the world changes under it.
inline constexpr int anchor_tile_level = render::tree_lattice_level - 4;

// The tile's trees, each sampled from the world at its own spot, relative to `origin`: the one
// placement, for the server to share one day. Pure, and deterministic for a fixed world. Needles
// take their colour from their biome's ground colour.
[[nodiscard]] std::vector<render::TreeAnchor> tile_anchors(const planet::World& world,
                                                           planet::CellId tile,
                                                           WorldPosition origin,
                                                           const BiomePalette& needles);

// Builds and keeps the tiles within reach of the eye, each bound to a slot of an anchor pool.
class TreeAnchors {
public:
    // Trees take their needles' colour from their biome's ground colour.
    explicit TreeAnchors(const BiomePalette& palette);

    // Keeps the tiles within `reach` metres of the eye built and staged, a few new tiles a call,
    // nearest first; tiles well beyond the reach give their slots back. The world must not change
    // during the call, and the frame's last submit must have finished.
    Result<> update(render::AnchorPool& pool, std::uint32_t frame, const planet::World& world,
                    WorldPosition eye, double reach, int builds = 2);

    // A cell of the world changed: the tiles over it are dropped, to be rebuilt by the next
    // update.
    void changed(render::AnchorPool& pool, planet::CellId cell);

    // The held tiles as draws, each binned to the finest detail its nearest ground needs.
    [[nodiscard]] std::vector<render::StandDraw> draws(WorldPosition eye,
                                                       const GrassBudget& budget = {}) const;

    [[nodiscard]] std::size_t size() const noexcept { return tiles_.size(); }
    // Whether the last update left nothing more to build within its reach.
    [[nodiscard]] bool settled() const noexcept { return settled_; }

private:
    struct Tile {
        std::uint32_t slot = 0;
        std::uint32_t count = 0;
        // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
        WorldPosition origin = {};
    };

    std::array<Vec3, planet::biome_count> needles_{};
    std::map<planet::CellId, Tile> tiles_;
    bool settled_ = false;
};

} // namespace hog::client
