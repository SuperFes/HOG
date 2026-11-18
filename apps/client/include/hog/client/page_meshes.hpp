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
// or a crack between pages.
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
    // Wants the held pages drawn as `drawn` has them, meshing what's changed, nearest `eye`
    // first. Precondition: settled(), and every page drawn held with a shape tree.
    void want(const HeldPages& held, const std::vector<DrawnPage>& drawn, DVec3 eye);
    // Meshes on; what's to be drawn since last asked, and what's gone.
    [[nodiscard]] GroundChanges collect();
    // Meshes all that's wanted, waiting for it.
    void finish();

    [[nodiscard]] std::size_t in_flight() const noexcept {
        return queued_.size() + working_.size();
    }

private:
    enum class Stage : std::uint8_t { Settled, Pages, Seams };

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

    // Starts what's queued, as many as may be in flight.
    void start();
    // Takes in what's meshed, waiting for it all if `wait`.
    void gather(bool wait);
    // On to seams once every page is meshed, and to drawing once every seam is.
    void advance();
    [[nodiscard]] std::unique_ptr<jobs::Async<Meshed>> mesh_page(grid::Key root) const;
    [[nodiscard]] std::unique_ptr<jobs::Async<Meshed>> mesh_seam(grid::Key root) const;
    // Puts what's wanted in place of what's drawn.
    void show();

    grid::Grid grid_;
    jobs::JobSystem* jobs_;
    Stage stage_ = Stage::Settled;
    std::map<grid::Key, Drawing> drawn_;
    std::map<grid::Key, Drawing> wanted_;
    std::map<grid::Key, std::vector<grid::Key>> touching_; // of those wanted
    std::deque<Queued> queued_;                            // nearest first
    std::vector<Work> working_;
    std::size_t most_working_;
    GroundChanges changes_;
};

} // namespace hog::client
