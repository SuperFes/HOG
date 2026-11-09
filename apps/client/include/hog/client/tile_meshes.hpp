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

// A tile as sent: where it stands, and its voxels.
struct SentTree {
    globe::TileKey key;
    std::shared_ptr<const globe::VoxelTree> tree;
};
// Fails on bytes that aren't a tile, or a tile off the planet's layers.
[[nodiscard]] Result<SentTree> sent_tree(const protocol::SentTile& sent,
                                         const globe::Layers& layers);

// The tiles the server sends, held as they come and meshed on the jobs, for the scene to draw.
// A tile is drawn until the finer tiles sent after it are meshed all through it, and finer tiles
// until one sent over them is meshed, so the ground never shows a hole between levels. What's
// drawn is worked out only about what's changed: a tile come, or meshed.
class TileMeshes {
public:
    // The jobs must outlive it.
    TileMeshes(const globe::Planet& planet, const globe::Layers& layers, jobs::JobSystem& jobs);
    TileMeshes(const TileMeshes&) = delete;
    TileMeshes& operator=(const TileMeshes&) = delete;
    TileMeshes(TileMeshes&&) = delete;
    TileMeshes& operator=(TileMeshes&&) = delete;
    ~TileMeshes();

    // Holds the tile, to be meshed at the next collect; fails on bytes that aren't a tile.
    Result<void> take(const protocol::SentTile& sent);
    // Precondition: from sent_tree, for these layers.
    void take(const SentTree& sent);
    // Waits for the meshing in flight.
    void finish();
    // What's to be drawn since last asked, and what's gone.
    [[nodiscard]] GroundChanges collect();

    [[nodiscard]] std::size_t in_flight() const noexcept {
        return working_.size() + waiting_.size();
    }
    [[nodiscard]] bool holds(globe::TileKey key) const { return held_.contains(key); }
    [[nodiscard]] bool drawn(globe::TileKey key) const;
    // Metres from `eye` within which the ground is drawn with no hole in it, up to `metres`; all
    // round the planet once the whole of it is.
    [[nodiscard]] double covered(DVec3 eye, double metres) const;

private:
    struct Held {
        std::shared_ptr<const globe::VoxelTree> tree;
        std::uint64_t arrived = 0; // order taken in
        std::shared_ptr<const terrain::BlockMesh> ground;
        std::shared_ptr<const terrain::BlockMesh> water;
        bool meshed = false;
        bool shown = false;
        bool stale = false; // a tile sent over it since, which it's drawn for till that's meshed
    };
    struct Work {
        globe::TileKey key;
        std::uint64_t arrived = 0;
        std::unique_ptr<jobs::Async<ColumnDrawn>> job;
    };
    // Coarser first, so a tile's decided after those over it.
    struct CoarseFirst {
        bool operator()(globe::TileKey a, globe::TileKey b) const noexcept {
            return a.level != b.level ? a.level > b.level : a < b;
        }
    };

    // Meshes the tile on the jobs, the tiles beside it as held now.
    void mesh(globe::TileKey key);
    // Takes the meshing done in, waiting for it all if `wait`.
    void gather(bool wait);
    // The tiles held under `key`.
    [[nodiscard]] std::vector<globe::TileKey> under(globe::TileKey key) const;
    // Whether every child of the cube is filled: drawn right through by fresh meshed tiles.
    [[nodiscard]] bool children_filled(globe::TileKey key) const;
    // The tile is filled, and so is each cube over it all of whose children now are.
    void fill(globe::TileKey key);
    // The cube isn't filled, nor any over it that was filled only through it.
    void unfill(globe::TileKey key);
    // Whether a tile held over `key` is drawn, or fresh and meshed, so drawn in its place.
    [[nodiscard]] bool under_drawn(globe::TileKey key) const;
    // Whether a tile held over `key`, fresh, meshed and sent after it, has replaced it.
    [[nodiscard]] bool replaced(globe::TileKey key, const Held& held) const;
    // Works out again whether the tile is dropped or drawn, and those under it where that
    // changes what they're drawn for.
    void decide(globe::TileKey key);
    void drop(globe::TileKey key);
    [[nodiscard]] bool in_shell(globe::TileKey key) const noexcept;
    // The face and the place on its level in Morton order, so the cubes under one are a run.
    [[nodiscard]] static std::uint64_t code(globe::TileKey key) noexcept;

    globe::Layers layers_;
    std::optional<double> sea_;
    jobs::JobSystem* jobs_;
    std::map<globe::TileKey, Held> held_;
    std::vector<std::map<std::uint64_t, globe::TileKey>> by_level_; // held, by code
    std::set<globe::TileKey> filled_;
    std::vector<globe::TileKey> roots_;
    std::set<globe::TileKey> waiting_;              // to mesh at the next collect
    std::set<globe::TileKey, CoarseFirst> touched_; // to decide at the next collect
    std::set<globe::TileKey> bare_;                 // held with nothing drawn for it yet
    std::vector<Work> working_;
    // Meshing no longer wanted, kept till done so it's never waited on.
    std::vector<std::unique_ptr<jobs::Async<ColumnDrawn>>> abandoned_;
    std::uint64_t taken_ = 0;
    // What's to be drawn under each key since last collected, the last word on each: shown, or
    // gone where none.
    std::map<std::uint64_t, std::optional<GroundChanges::Shown>> changes_;
};

} // namespace hog::client
