#pragma once

#include <hog/client/ground_changes.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/client/transvoxel.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace hog::client {

// The key a block's mesh is drawn under: past every tile's key.
[[nodiscard]] std::uint64_t block_key(grid::Key node) noexcept;

// The ground as drawn from a shape mesh: each vertex showing the material most of it shows,
// blended across triangles whose corners differ.
[[nodiscard]] terrain::BlockMesh block_of(const ShapeMesh& mesh);

// The held ground drawn in blocks as ground_blocks chooses them, with the water standing in each,
// meshed on the jobs and kept until how a block's drawn or a piece it reads changes, for the scene
// to draw. The water bodies a piece marks come before it, so they're held by then. What's drawn
// changes all at once, once all that's wanted is meshed, so the ground never shows a hole. All
// but taking in what the jobs give is done on them, so a frame never waits on it.
// A block as it's drawn: its node, how it's cut, the pieces its mesh was made from, and its
// triangles.
struct DrawnBlock {
    Block block;
    std::vector<grid::Key> reads;
    std::size_t ground_triangles = 0;
    std::size_t water_triangles = 0;
};

class BlockMeshes {
public:
    // Blocks of `cells` levels of cells. The jobs must outlive it.
    BlockMeshes(const grid::Grid& grid, jobs::JobSystem& jobs, std::uint8_t cells);
    BlockMeshes(const BlockMeshes&) = delete;
    BlockMeshes& operator=(const BlockMeshes&) = delete;
    BlockMeshes(BlockMeshes&&) = delete;
    BlockMeshes& operator=(BlockMeshes&&) = delete;
    ~BlockMeshes();

    // Whether what was last wanted is drawn, so another drawing may be wanted.
    [[nodiscard]] bool settled() const noexcept { return stage_ == Stage::Settled; }
    // Wants the held ground drawn as seen from `eye`, meshing what's changed, nearest first.
    // Precondition: settled().
    void want(const HeldPieces& held, DVec3 eye);
    // Meshes on; what's to be drawn since last asked, and what's gone.
    [[nodiscard]] GroundChanges collect();
    // Meshes all that's wanted, waiting for it.
    void finish();

    // Each block drawn now, in the order of its node.
    [[nodiscard]] std::vector<DrawnBlock> drawn() const;

    [[nodiscard]] std::size_t in_flight() const noexcept {
        return queued_.size() + working_.size() + (planning_ ? 1 : 0) + (showing_ ? 1 : 0);
    }

private:
    // Planning what's wanted, meshing blocks, then showing them.
    enum class Stage : std::uint8_t { Settled, Planning, Meshing, Showing };

    // How a block is drawn: all its mesh depends on.
    struct Look {
        Block block;
        std::vector<PiecesSnapshot::Entry> reads;

        [[nodiscard]] bool same(const Look& other) const noexcept;
    };
    struct Meshed {
        std::shared_ptr<const terrain::BlockMesh> ground;
        std::shared_ptr<const terrain::BlockMesh> water;
    };
    struct Drawing {
        Look look;
        Meshed mesh;
    };
    using Drawings = std::map<grid::Key, Drawing>;
    struct Plan {
        Drawings wanted;
        std::vector<grid::Key> queued; // nearest first
        std::shared_ptr<const PiecesSnapshot> pieces;
        std::shared_ptr<const grid::WaterBodies> bodies;
    };
    struct Shown {
        Drawings drawn;
        GroundChanges changes;
    };
    struct Work {
        grid::Key node;
        std::unique_ptr<jobs::Async<Meshed>> job;
    };

    // Starts what's queued, as many as may be in flight.
    void start();
    // Takes in what's meshed, waiting for it all if `wait`.
    void gather(bool wait);
    // On through the stages as each is done, waiting for a plan or what's shown if `wait`.
    void advance(bool wait);
    [[nodiscard]] std::unique_ptr<jobs::Async<Shown>> show();

    grid::Grid grid_;
    jobs::JobSystem* jobs_;
    std::uint8_t cells_;
    Stage stage_ = Stage::Settled;
    // Read by the job planning, and only changed once it's done.
    Drawings drawn_;
    Drawings wanted_;
    std::shared_ptr<const PiecesSnapshot> pieces_; // what's wanted is meshed from
    std::shared_ptr<const grid::WaterBodies> bodies_;
    std::deque<grid::Key> queued_; // nearest first
    std::vector<Work> working_;
    std::size_t most_working_;
    std::unique_ptr<jobs::Async<Plan>> planning_;
    std::unique_ptr<jobs::Async<Shown>> showing_;
    GroundChanges changes_;
};

} // namespace hog::client
