#pragma once

#include <hog/client/anchors.hpp>
#include <hog/client/ground.hpp>
#include <hog/client/tiles.hpp>
#include <hog/core/result.hpp>
#include <hog/flora/bushes.hpp>
#include <hog/flora/fates.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/world.hpp>
#include <hog/render/anchors.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace hog::client {

// Tiles of anchored bushes, as the trees' tiles: each a level-12 cell holding its 16 by 16
// lattice cells' bushes, sampled once from the world itself, so every bush keeps its spot, kind
// and size at every terrain level, until the world or its woods change under it.
inline constexpr int bush_tile_level = flora::bush_lattice_level - 4;

// The tile's bushes, each sampled from the world at its own spot, relative to `origin`, growing
// as `fates` have the woods about them fare. Pure, and deterministic for a fixed world and fates.
// Leaves take their colour from their biome's ground colour.
[[nodiscard]] std::vector<render::BushAnchor>
tile_bushes(const planet::World& world, planet::CellId tile, WorldPosition origin,
            const BiomePalette& ground, const flora::Fates& fates = flora::Fates{});

// Builds and keeps the bush tiles within reach of the eye, each bound to a slot of an anchor pool.
class BushAnchors {
public:
    // Tiles are built on `jobs` when given, else in place.
    explicit BushAnchors(const BiomePalette& palette, jobs::JobSystem* jobs = nullptr);

    // As TreeAnchors::update.
    Result<> update(render::AnchorPool& pool, std::uint32_t frame, const planet::World& world,
                    WorldPosition eye, double reach, int builds = 4);

    // As TreeAnchors::changed and TreeAnchors::fare.
    void changed(render::AnchorPool& pool, planet::CellId cell);
    void fare(std::shared_ptr<const flora::Fates> fates, std::span<const planet::CellId> changed);

    // The held tiles in sight with ground within `reach` of the eye.
    [[nodiscard]] std::vector<render::StandDraw> draws(const Sight& sight, double reach) const;

    [[nodiscard]] std::size_t size() const noexcept { return tiles_.tiles().size(); }
    [[nodiscard]] bool settled() const noexcept { return tiles_.settled(); }
    void finish() { tiles_.finish(); }

private:
    struct Tile {
        std::uint32_t slot = 0;
        std::uint32_t count = 0;
        // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
        WorldPosition origin = {};
        bool stale = false;
    };

    BiomePalette leaves_{};
    jobs::JobSystem* jobs_ = nullptr;
    std::shared_ptr<const flora::Fates> fates_ = std::make_shared<const flora::Fates>();
    tiles::Keeper<Tile, std::vector<render::BushAnchor>> tiles_{bush_tile_level};
};

} // namespace hog::client
