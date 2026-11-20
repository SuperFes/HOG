#pragma once

#include <hog/client/ground_changes.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/client/transvoxel.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/z_order.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace hog::client {

// The key a block's mesh is drawn under: past every tile's key.
[[nodiscard]] std::uint64_t block_key(grid::Key node) noexcept;

// The ground as drawn from a shape mesh: each vertex showing the material most of it shows,
// blended across triangles whose corners differ.
[[nodiscard]] terrain::BlockMesh block_of(const ShapeMesh& mesh);

// A block as it's drawn: its node, how it's cut, the pieces its mesh was made from, and its
// triangles.
struct DrawnBlock {
    Block block;
    std::vector<grid::Key> reads;
    std::size_t ground_triangles = 0;
    std::size_t water_triangles = 0;
};

// The held ground drawn in blocks as ground_blocks chooses them, with the water standing in each,
// meshed on the jobs nearest first and kept until how a block's drawn or a piece it reads changes,
// for the scene to draw. The water bodies a piece marks come before it, so they're held by then.
// Each block is shown once it's meshed, where that opens no hole: where nothing's drawn yet, in
// place of the block drawn at its node, or of the finer ones drawn under it; but finer blocks in
// place of a coarser one only once every block wanted under it is meshed, all at once. Drawn
// blocks no longer wanted anywhere go once all that's wanted is shown. All but taking in what
// the jobs give is done on them, so a frame never waits on it.
class BlockMeshes {
public:
    // Blocks of `cells` levels of cells. The jobs must outlive it.
    BlockMeshes(const grid::Grid& grid, jobs::JobSystem& jobs, std::uint8_t cells);
    BlockMeshes(const BlockMeshes&) = delete;
    BlockMeshes& operator=(const BlockMeshes&) = delete;
    BlockMeshes(BlockMeshes&&) = delete;
    BlockMeshes& operator=(BlockMeshes&&) = delete;
    ~BlockMeshes();

    // Whether all that was last wanted is drawn.
    [[nodiscard]] bool settled() const noexcept {
        return !planning_ && queued_.empty() && working_.empty() && pending_.empty();
    }
    // Whether what's wanted is still being worked out, so another drawing can't be wanted yet.
    [[nodiscard]] bool planning() const noexcept { return planning_ != nullptr; }
    // Wants the held ground drawn as seen from `eye`, in place of what was wanted, meshing what's
    // changed, nearest first; blocks meshing that are still wanted as they are go on.
    // Precondition: !planning().
    void want(const HeldPieces& held, DVec3 eye);
    // Meshes on; what's to be drawn since last asked, and what's gone.
    [[nodiscard]] GroundChanges collect();
    // Meshes all that's wanted, waiting for it.
    void finish();

    // Each block drawn now, in the order of its node.
    [[nodiscard]] std::vector<DrawnBlock> drawn() const;

    // How much work drawing's taken: the plans made, the last one's milliseconds, the blocks
    // meshed, and of those how many were wanted otherwise by the time they were done.
    struct Stats {
        std::size_t plans = 0;
        double last_plan_ms = 0.0;
        std::size_t meshed = 0;
        std::size_t wasted = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] std::size_t in_flight() const noexcept {
        return queued_.size() + working_.size() + pending_.size() + (planning_ ? 1 : 0);
    }

private:
    // How a block is drawn: all its mesh depends on.
    struct Look {
        Block block;
        std::vector<PiecesSnapshot::Entry> reads;

        [[nodiscard]] bool same(const Look& other) const noexcept;
    };
    struct Meshed {
        std::shared_ptr<const terrain::BlockMesh> ground;
        std::shared_ptr<const terrain::BlockMesh> water;

        friend bool operator==(const Meshed&, const Meshed&) noexcept = default;
    };
    // A block and its mesh, none till it's meshed.
    struct Drawing {
        Look look;
        std::optional<Meshed> mesh;
    };
    using Drawings = grid::ZMap<Drawing>;
    struct Plan {
        Drawings wanted;
        std::vector<grid::Key> queued; // nearest first
        std::shared_ptr<const grid::WaterBodies> bodies;
        double milliseconds = 0.0;
    };
    struct Work {
        grid::Key node;
        Look look;
        std::unique_ptr<jobs::Async<Meshed>> job;
    };

    // Starts what's queued, as many as may be in flight.
    void start();
    // Takes in what's meshed, waiting for it all if `wait`.
    void gather(bool wait);
    // Takes in a plan once it's made, waiting for it if `wait`.
    void take_plan(bool wait);
    // Shows what's meshed where it opens no hole, and once all that's wanted is shown, lets go of
    // what's drawn and not wanted.
    void publish();
    // Shows the wanted block at `node` in place of what's drawn at it or under it.
    void show(grid::Key node);
    // Shows every block wanted under the drawn `coarser` in its place, once all are meshed.
    void refine(grid::Key coarser);
    // Lets go of what's drawn at `node`.
    void hide(grid::Key node);

    grid::Grid grid_;
    jobs::JobSystem* jobs_;
    std::uint8_t cells_;
    Drawings drawn_;
    Drawings wanted_;
    std::shared_ptr<const grid::WaterBodies> bodies_;
    std::deque<grid::Key> queued_; // nearest first
    std::vector<Work> working_;
    // Wanted blocks meshed and not yet shown.
    std::set<grid::Key, grid::ZOrder> pending_;
    std::size_t most_working_;
    std::unique_ptr<jobs::Async<Plan>> planning_;
    GroundChanges changes_;
    Stats stats_;
};

} // namespace hog::client
