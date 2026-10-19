#pragma once

#include <hog/client/ground.hpp>
#include <hog/core/result.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/world.hpp>
#include <hog/render/anchors.hpp>
#include <hog/render/camera.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace hog::client {

// Tiles of anchored trees: each a level-10 cell holding its 16 by 16 lattice cells' trees,
// sampled once from the world itself — never the drawn ground — so every tree keeps its spot,
// height and lean at every terrain level, until the world changes under it.
inline constexpr int anchor_tile_level = render::tree_lattice_level - 4;

// A tile's trees are kept in blocks of 4 by 4 lattice cells, so the grown forest can be drawn,
// culled and detailed a stand of a few dozen metres at a time rather than a whole tile.
inline constexpr std::uint32_t tile_blocks = 16;

struct TileTrees {
    // Block by block, each block's in lattice order.
    std::vector<render::TreeAnchor> anchors;
    // Where each block's trees begin; the last entry is the count.
    std::array<std::uint32_t, tile_blocks + 1> starts{};
};

// The tile's trees, each sampled from the world at its own spot, relative to `origin`: the one
// placement, for the server to share one day. Pure, and deterministic for a fixed world. Needles
// take their colour from their biome's ground colour.
[[nodiscard]] TileTrees tile_anchors(const planet::World& world, planet::CellId tile,
                                     WorldPosition origin, const BiomePalette& needles);

// What the eye can see: a cone round its forward, wide enough to hold the picture's corners.
struct Sight {
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition eye = {};
    DVec3 forward{0.0, 0.0, 1.0}; // unit length
    double half_angle = 4.0;      // radians; past pi, every way
};

// The cone a camera sees through a picture of that aspect.
[[nodiscard]] Sight sight_of(const render::Camera& camera, double aspect);

// Whether a ball `radius` metres round a point `from_eye` might fall within sight.
[[nodiscard]] bool within_sight(const Sight& sight, DVec3 from_eye, double radius);

// The trees a frame draws, by tier.
struct TreeCounts {
    std::size_t tiles = 0;   // held, built from the world
    std::size_t stamped = 0; // anchors sent to the stamps, which hold off near the eye
    std::size_t grown = 0;   // anchors sent to the grown forest
};

// What a frame's draws send, counted.
[[nodiscard]] std::size_t anchors_in(std::span<const render::StandDraw> draws) noexcept;

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

    // The held tiles in sight as whole-tile draws, for the stamps.
    [[nodiscard]] std::vector<render::StandDraw> draws(const Sight& sight) const;

    // The blocks in sight within the grown forest's reach, each binned to the finest detail its
    // nearest tree needs.
    [[nodiscard]] std::vector<render::StandDraw> forest_draws(const Sight& sight,
                                                              const GrassBudget& budget = {}) const;

    [[nodiscard]] std::size_t size() const noexcept { return tiles_.size(); }
    // Whether the last update left nothing more to build within its reach.
    [[nodiscard]] bool settled() const noexcept { return settled_; }

private:
    struct Block {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        Vec3 centre{0.0F};  // from the tile's origin
        float reach = 0.0F; // metres from the centre to its farthest tree's root
    };

    struct Tile {
        std::uint32_t slot = 0;
        std::uint32_t count = 0;
        // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
        WorldPosition origin = {};
        std::array<Block, tile_blocks> blocks{};
    };

    static Tile held_tile(std::uint32_t slot, WorldPosition origin, const TileTrees& trees);

    std::array<Vec3, planet::biome_count> needles_{};
    std::map<planet::CellId, Tile> tiles_;
    bool settled_ = false;
};

} // namespace hog::client
