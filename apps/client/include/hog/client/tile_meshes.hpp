#pragma once

#include <hog/client/column_meshes.hpp>
#include <hog/client/ground_changes.hpp>
#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/voxel_tree.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace hog::client {

// The key a tile's meshes are drawn under.
[[nodiscard]] std::uint64_t tile_key(globe::TileKey key) noexcept;

// The tiles the server sends, held as they come and meshed on the jobs, for the scene to draw.
// A tile is drawn until the finer tiles sent after it are meshed all through it, and finer tiles
// until one sent over them is meshed, so the ground never shows a hole between levels.
class TileMeshes {
public:
    // The jobs must outlive it.
    TileMeshes(const globe::Planet& planet, const globe::Layers& layers, jobs::JobSystem& jobs);

    // Holds the tile, to be meshed at the next collect; fails on bytes that aren't a tile.
    Result<void> take(const protocol::SentTile& sent);
    // Waits for the meshing in flight.
    void finish();
    // What's to be drawn since last asked, and what's gone.
    [[nodiscard]] GroundChanges collect();

    [[nodiscard]] std::size_t in_flight() const noexcept {
        return working_.size() + waiting_.size();
    }
    [[nodiscard]] bool holds(globe::TileKey key) const { return held_.contains(key); }
    [[nodiscard]] bool drawn(globe::TileKey key) const;
    // Metres from `eye` within which all that's held is drawn, up to `metres`.
    [[nodiscard]] double covered(DVec3 eye, double metres) const;

private:
    struct Held {
        std::shared_ptr<const globe::VoxelTree> tree;
        std::uint64_t arrived = 0; // order taken in
        std::shared_ptr<const terrain::BlockMesh> ground;
        std::shared_ptr<const terrain::BlockMesh> water;
        bool meshed = false;
        bool shown = false;
    };
    struct Work {
        globe::TileKey key;
        std::uint64_t arrived = 0;
        std::unique_ptr<jobs::Async<ColumnDrawn>> job;
    };

    // Meshes the tile on the jobs, the tiles beside it as held now.
    void mesh(globe::TileKey key);
    // Takes the meshing done in, waiting for it all if `wait`.
    void gather(bool wait);
    // Each held tile's meshed cover: the oldest arrival of finer tiles meshed all through it,
    // none where they aren't.
    [[nodiscard]] std::map<globe::TileKey, std::uint64_t> covers() const;
    // The oldest cover of the cube's children, none where any isn't covered.
    [[nodiscard]] std::uint64_t
    finer_cover(globe::TileKey key, const std::map<globe::TileKey, std::uint64_t>& cover) const;
    // Whether a tile held over `key` is meshed and came after it.
    [[nodiscard]] bool replaced(globe::TileKey key, const Held& held) const;
    // Whether a tile held over `key` is meshed: it's drawn instead.
    [[nodiscard]] bool under_meshed(globe::TileKey key) const;
    [[nodiscard]] bool in_shell(globe::TileKey key) const noexcept;
    void drop(globe::TileKey key);

    globe::Layers layers_;
    std::optional<double> sea_;
    jobs::JobSystem* jobs_;
    std::map<globe::TileKey, Held> held_;
    std::set<globe::TileKey> waiting_; // to mesh at the next collect
    std::vector<Work> working_;
    std::uint64_t taken_ = 0;
    GroundChanges changes_;
};

} // namespace hog::client
