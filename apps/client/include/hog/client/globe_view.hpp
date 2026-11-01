#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/surface_mesh.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/terrain/blocks.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace hog::client {

// The planet drawn as the globe: a preset by name or a planet file, Earth's knobs if neither.
// A preset, or Earth, takes the seed and radius of the world it's drawn in place of, so the sky
// fits it.
[[nodiscard]] Result<globe::Planet> globe_planet(std::optional<std::string_view> named,
                                                 std::uint64_t seed, double radius);

// The voxel world drawn in place of the ground, nothing yet changed in it.
struct GlobeWorld {
    explicit GlobeWorld(const globe::Planet& planet)
        : generator{planet}, store{generator.layers()}, world{generator, store} {}
    GlobeWorld(const GlobeWorld&) = delete;
    GlobeWorld& operator=(const GlobeWorld&) = delete;
    GlobeWorld(GlobeWorld&&) = delete;
    GlobeWorld& operator=(GlobeWorld&&) = delete;
    ~GlobeWorld() = default;

    globe::Generator generator;
    globe::VoxelStore store;
    globe::World world;
};

// Metres the generated ground stands over the planet's radius under a point.
[[nodiscard]] double globe_ground(const globe::Generator& generator, DVec3 at);

// How a material shows until the globe is textured.
[[nodiscard]] Vec3 surface_colour(globe::Material material);

// Chunks' surfaces as one mesh of blocks laid from `origin`, in metres from the planet's centre.
[[nodiscard]] terrain::BlockMesh globe_blocks(std::span<const globe::SurfaceMesh* const> meshes,
                                              DVec3 origin);

// The globe's surfaces are drawn this far round the eye, and sought anew once it has gone this
// far, a chunk's width.
inline constexpr double globe_reach = 160.0; // metres
inline constexpr double globe_relay = 16.0;  // metres

// The globe's surfaces round the eye, meshed a chunk at a time in the background on the pool,
// nearest first, and those out of reach let go. For the thread that made it; the world must
// outlive it, unchanged while chunks are in flight.
class GlobeStream {
public:
    GlobeStream(const globe::World& world, jobs::JobSystem& jobs, double reach = globe_reach);

    // Wants the surfaces round the unit direction `eye`.
    void follow(DVec3 eye);
    // Chunks still to mesh, or meshed and not yet handed over.
    [[nodiscard]] bool in_flight() const noexcept;
    // The surfaces as they now stand, if chunks have come or gone since the last; handed over as
    // they arrive in growing batches, so the mesh isn't remade for every chunk.
    [[nodiscard]] std::optional<terrain::BlockMesh> collect();
    // Waits for every chunk wanted, and hands the surfaces over if they changed.
    [[nodiscard]] std::optional<terrain::BlockMesh> finish();

    [[nodiscard]] std::size_t chunks() const noexcept { return meshed_.size(); }

private:
    void start_more();
    void take(std::size_t working);
    [[nodiscard]] terrain::BlockMesh mesh() const;

    const globe::World* world_;
    jobs::JobSystem* jobs_;
    double reach_;
    std::optional<DVec3> around_;
    std::vector<globe::Cell> wanted_; // farthest first
    std::vector<std::pair<globe::Cell, std::unique_ptr<jobs::Async<globe::SurfaceMesh>>>> working_;
    std::map<globe::Cell, globe::SurfaceMesh> meshed_;
    std::size_t fresh_ = 0; // chunks meshed or let go since the surfaces were last handed over
};

} // namespace hog::client
