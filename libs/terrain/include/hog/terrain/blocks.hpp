#pragma once

#include <hog/core/math.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/materials.hpp>
#include <hog/planet/volume.hpp>
#include <hog/planet/world.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace hog::terrain {

// A corner of a surface, textured by the tiles of the triangle it's drawn in.
struct BlockVertex {
    Vec3 position{0.0F}; // metres from the mesh's origin
    Vec3 normal{0.0F};   // unit, planet-centred, out of the side
    // The tiles at the triangle's three corners, and which of them this corner is. A vertex shared
    // between triangles names its own tile three times, so whichever triangle draws it finds it.
    std::array<std::uint8_t, 3> tiles{};
    std::uint8_t corner = 0;

    friend bool operator==(const BlockVertex&, const BlockVertex&) = default;
};

// Triangles of three indices each, anticlockwise seen from outside.
struct BlockMesh {
    WorldPosition origin;
    std::vector<BlockVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// Gives each triangle whose corners wear different tiles three corners of its own, naming all
// three tiles, so they blend across it. Every vertex must name one tile three times.
void mix_corners(BlockMesh& mesh);

// How a material shows when drawn as blocks; its tile is its material's number.
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

// Each ring out from the eye has columns this many levels coarser than the one inside it, reaching
// as many times farther as they are across.
inline constexpr int ring_step = 3;
// Under the rings lies the whole planet in columns at this level, a bounded count whatever its
// size, in shells about this many metres tall, so its mountains still step.
inline constexpr int overview_level = 6;
inline constexpr double overview_shell = 250.0; // metres

// The ground and sea of the whole planet on a planet of `radius`, as the world's terrain has them,
// filled into a volume in columns at overview_level. It stands in for a world made in the volume
// until there is one.
[[nodiscard]] planet::Volume planet_volume(const planet::World& world, double radius);

// `planet` with rings of ever finer columns filled in round the unit direction `around`, down to
// block_size within block_reach, all shown as blocks from where `around` meets the planet's radius.
[[nodiscard]] BlockMesh ground_blocks(const planet::Volume& planet, const planet::World& world,
                                      DVec3 around, double radius);
// The same over the whole planet filled in anew.
[[nodiscard]] BlockMesh ground_blocks(const planet::World& world, DVec3 around, double radius);

// Fills the ground in as blocks round the eye in the background on the pool, anew whenever the eye
// has gone block_relay from where it was last filled round, for the thread that made it. The whole
// planet under the rings is filled once and kept. One is in flight at a time; the world must
// outlive it, unchanged while one is.
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
    struct Filled {
        BlockMesh mesh;
        std::shared_ptr<const planet::Volume> planet;
    };

    void start(DVec3 around);
    [[nodiscard]] BlockMesh take();

    const planet::World* world_;
    jobs::JobSystem* jobs_;
    double radius_;
    std::shared_ptr<const planet::Volume> planet_;
    std::optional<DVec3> wanted_;
    std::optional<DVec3> started_;
    std::unique_ptr<jobs::Async<Filled>> working_;
};

} // namespace hog::terrain
