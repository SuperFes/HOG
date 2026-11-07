#pragma once

#include <hog/client/ground_changes.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_mesh.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace hog::client {

// A tile column's faces as drawn: its ground, and its water still in lakes and pools.
struct ColumnDrawn {
    terrain::BlockMesh ground;
    terrain::BlockMesh water;
};

// What marks the sea's own surfaces among still water: the corner byte of each of their vertices.
inline constexpr std::uint8_t sea_corner = 1;

// The faces split into ground and water: a liquid's faces at `sea` metres from the planet's
// centre, within a voxel `thickness`, are the sea's own, marked so for its waves to be drawn.
[[nodiscard]] ColumnDrawn drawn_of(const globe::VoxelMesh& mesh, std::optional<double> sea,
                                   double thickness);

// The key a tile column's meshes are drawn under.
[[nodiscard]] std::uint64_t column_key(globe::Site corner) noexcept;

// The tile columns the world holds, meshed on the jobs as they come and change, at the level
// each is held, for the scene to draw.
class ColumnMeshes {
public:
    // The world and jobs must outlive it.
    ColumnMeshes(const globe::World& world, jobs::JobSystem& jobs);

    // The tile columns at these corners changed: meshed again as now held, or gone where none is.
    void changed(std::span<const globe::Site> corners);
    // Has the tile columns held within `metres` of `eye`, from the planet's centre, meshed, and
    // those meshed further off gone: for a world held whole, not sent.
    void follow(DVec3 eye, double metres);
    // Waits for the meshing in flight: before the world changes under it.
    void finish();
    // What's done meshing since last asked, and what's gone.
    [[nodiscard]] GroundChanges collect();
    [[nodiscard]] std::size_t in_flight() const noexcept { return working_.size(); }

private:
    struct Work {
        globe::Site corner;
        std::unique_ptr<jobs::Async<ColumnDrawn>> job;
    };

    // Takes the work's meshes into what's shown.
    void show(Work& work);

    const globe::World* world_;
    jobs::JobSystem* jobs_;
    std::vector<Work> working_;
    GroundChanges changes_;
    std::set<globe::Site> meshed_; // corners asked for and not gone since
};

} // namespace hog::client
