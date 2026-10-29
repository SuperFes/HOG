#pragma once

#include <hog/core/math.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/materials.hpp>
#include <hog/planet/volume.hpp>
#include <hog/planet/world.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hog::terrain {

// A corner of a leaf's side, flat-lit by the side's normal.
struct BlockVertex {
    Vec3 position{0.0F}; // metres from the mesh's origin
    Vec3 normal{0.0F};   // unit, planet-centred, out of the side
    Vec3 colour{0.0F};   // linear

    friend bool operator==(const BlockVertex&, const BlockVertex&) = default;
};

// Sides of the volume's leaves as quads, two triangles of three indices each.
struct BlockMesh {
    WorldPosition origin;
    std::vector<BlockVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// How a material shows when drawn as blocks.
[[nodiscard]] Vec3 block_colour(planet::Material material);

// Adds to `mesh` the sides showing in one region of the volume: each leaf's sides that face
// something clearer than it (air over water over everything else).
// Precondition: `region` is a column at the volume's region level.
void add_blocks(BlockMesh& mesh, const planet::Volume& volume, double radius,
                planet::CellId region);

// The same, laid from where the region's middle meets the planet's radius.
[[nodiscard]] BlockMesh block_mesh(const planet::Volume& volume, double radius,
                                   planet::CellId region);

// The ground is filled in as blocks this far round the eye, and anew once the eye has gone this
// far from where it last was.
inline constexpr double block_reach = 3'000.0; // metres
inline constexpr double block_relay = 500.0;   // metres
// Columns about this many metres across, and shells about a quarter of it.
inline constexpr double block_size = 50.0; // metres

// The ground and sea round the unit direction `around` on a planet of `radius`, as the world's
// terrain has them, filled into a volume and shown as blocks from where `around` meets the
// planet's radius. It stands in for a world made in the volume until there is one.
[[nodiscard]] BlockMesh ground_blocks(const planet::World& world, DVec3 around, double radius);

// Fills the ground in as blocks round the eye in the background on the pool, anew whenever the eye
// has gone block_relay from where it was last filled round, for the thread that made it. One is in
// flight at a time; the world must outlive it, unchanged while one is.
class BlockStream {
public:
    BlockStream(const planet::World& world, double radius, jobs::JobSystem& jobs) noexcept
        : world_(&world), jobs_(&jobs), radius_(radius) {}

    // Wants the blocks round the unit direction `eye`.
    void follow(DVec3 eye);
    [[nodiscard]] bool in_flight() const noexcept { return working_ != nullptr; }
    // The blocks filled in since the last call, if any.
    [[nodiscard]] std::optional<BlockMesh> collect();
    // Waits for what's in flight, and for what's wanted after it, and hands over the last.
    [[nodiscard]] std::optional<BlockMesh> finish();

private:
    void start(DVec3 around);

    const planet::World* world_;
    jobs::JobSystem* jobs_;
    double radius_;
    std::optional<DVec3> wanted_;
    std::optional<DVec3> started_;
    std::unique_ptr<jobs::Async<BlockMesh>> working_;
};

} // namespace hog::terrain
