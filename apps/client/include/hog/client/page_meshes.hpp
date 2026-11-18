#pragma once

#include <hog/client/drawn_pages.hpp>
#include <hog/client/ground_changes.hpp>
#include <hog/client/held_pages.hpp>
#include <hog/client/shape_mesh.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/page.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace hog::client {

// The key a page's mesh is drawn under, or its seam's: past every tile's key.
[[nodiscard]] std::uint64_t page_key(grid::Key root, bool seam) noexcept;

// The ground as drawn from a shape mesh: each vertex showing the material most of its leaf
// shows, blended across triangles whose corners differ.
[[nodiscard]] terrain::BlockMesh block_of(const ShapeMesh& mesh);

// The held pages as drawn, meshed on the jobs and kept until their shape trees or how they're
// drawn change, each page's seam until a page touching it changes, for the scene to draw. What's
// drawn changes all at once, once all that's wanted is meshed, so the ground never shows a hole
// or a crack between pages. All but taking in what the jobs give is done on them, so a frame
// never waits on it.
class PageMeshes {
public:
    // The jobs must outlive it.
    PageMeshes(const grid::Grid& grid, jobs::JobSystem& jobs);
    PageMeshes(const PageMeshes&) = delete;
    PageMeshes& operator=(const PageMeshes&) = delete;
    PageMeshes(PageMeshes&&) = delete;
    PageMeshes& operator=(PageMeshes&&) = delete;
    ~PageMeshes();

    // Whether what was last wanted is drawn, so another drawing may be wanted.
    [[nodiscard]] bool settled() const noexcept { return stage_ == Stage::Settled; }
    // Wants the held pages drawn as seen from `eye`, as drawn_pages has them, meshing what's
    // changed, nearest first. Precondition: settled().
    void want(const HeldPages& held, DVec3 eye);
    // The same, drawn as `drawn` has them. Precondition: every page drawn held with a shape tree.
    void want(const HeldPages& held, std::vector<DrawnPage> drawn, DVec3 eye);
    // Meshes on; what's to be drawn since last asked, and what's gone.
    [[nodiscard]] GroundChanges collect();
    // Meshes all that's wanted, waiting for it.
    void finish();

    [[nodiscard]] std::size_t in_flight() const noexcept {
        return queued_.size() + working_.size() + (planning_ ? 1 : 0) + (showing_ ? 1 : 0);
    }

private:
    // Planning what's wanted, meshing pages, linking seams, meshing them, then showing them.
    enum class Stage : std::uint8_t { Settled, Planning, Pages, Linking, Seams, Showing };

    // How a page is drawn: all its mesh depends on.
    struct Look {
        std::shared_ptr<const grid::Page> page;
        std::uint8_t levels = 0;
        std::vector<grid::Key> under;

        [[nodiscard]] bool same(const Look& other) const;
    };
    struct Meshed {
        std::shared_ptr<const ShapeMesh> shape; // its origin and border alone, for seams
        std::shared_ptr<const terrain::BlockMesh> block;
    };
    // What a seam reads: the meshes of the pages it touches, and its own, by their roots.
    using Reads = std::vector<std::pair<grid::Key, const ShapeMesh*>>;
    struct Drawing {
        Look look;
        Meshed own;
        Reads reads;
        std::shared_ptr<const terrain::BlockMesh> seam;
    };
    struct Work {
        grid::Key root;
        bool seam = false;
        std::unique_ptr<jobs::Async<Meshed>> job;
    };
    struct Queued {
        grid::Key root;
        bool seam = false;
    };
    using Drawings = std::map<grid::Key, Drawing>;
    // What's wanted, and what of it is still to mesh, nearest first.
    struct Plan {
        Drawings wanted;
        std::vector<Queued> queued;
    };
    struct Shown {
        Drawings drawn;
        GroundChanges changes;
    };

    void plan(std::vector<PagesSnapshot::Entry> pages, std::optional<std::vector<DrawnPage>> drawn,
              DVec3 eye);
    // Starts what's queued, as many as may be in flight.
    void start();
    // Takes in what's meshed, waiting for it all if `wait`.
    void gather(bool wait);
    // On through the stages as each is done, waiting for a plan or what's shown if `wait`.
    void advance(bool wait);
    [[nodiscard]] std::unique_ptr<jobs::Async<Meshed>> mesh_page(grid::Key root) const;
    [[nodiscard]] std::unique_ptr<jobs::Async<Meshed>> mesh_seam(grid::Key root) const;
    // The seams each page wanted reads, and those to mesh again.
    [[nodiscard]] std::unique_ptr<jobs::Async<Plan>> link();
    // What's wanted put in place of what's drawn.
    [[nodiscard]] std::unique_ptr<jobs::Async<Shown>> show();

    grid::Grid grid_;
    jobs::JobSystem* jobs_;
    Stage stage_ = Stage::Settled;
    // Read by the jobs planning and linking, and only changed once they're done.
    Drawings drawn_;
    Drawings wanted_;
    std::deque<Queued> queued_; // nearest first
    std::vector<Work> working_;
    std::size_t most_working_;
    std::unique_ptr<jobs::Async<Plan>> planning_;
    std::unique_ptr<jobs::Async<Shown>> showing_;
    GroundChanges changes_;
};

} // namespace hog::client
