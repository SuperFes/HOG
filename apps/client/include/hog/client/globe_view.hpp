#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/detail.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/surface_mesh.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/terrain/block_mesh.hpp>
#include <hog/terrain/material.hpp>

#include <array>
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

// What a material averages to, seen from afar.
[[nodiscard]] Vec3 surface_colour(globe::Material material);

// How each material looks up close, in Material's order, averaging to its surface_colour.
[[nodiscard]] std::array<terrain::Look, globe::material_count> globe_looks();

// Texels across a material's tile: about 1.5 cm each over the four metres a tile spans.
inline constexpr int material_size = 256;

// The tiles block meshes name: the globe's materials by number.
[[nodiscard]] std::vector<terrain::MaterialImage> block_tiles();

// A chunk's shaped surfaces as blocks, laid from the chunk's own origin, each triangle wearing
// its corners' materials; the liquid's surface is left out, as the sea is drawn over the ground.
[[nodiscard]] terrain::BlockMesh globe_blocks(const globe::SurfaceMesh& mesh);

// How the globe's detail falls off: each chunk as coarse as keeps it `ratio` of its widths from
// the eye, out to the horizon and no less than `least` round it.
struct GlobeDetail {
    double ratio = 3.0;
    double least = 1'000.0; // metres
};

// The chunks are sought anew once the eye has gone this far, or a twentieth of its height.
inline constexpr double globe_relay = 16.0; // metres

// The key a chunk's mesh is shown under.
[[nodiscard]] std::uint64_t globe_key(globe::Cell chunk);

// The globe's surfaces round the eye, coarser further off, meshed a chunk at a time in the
// background on the pool, nearest first. A chunk is meshed again when the detail of those beside
// it changes, and one no longer wanted is let go only once those drawn in its place are there, so
// the ground doesn't open. For the thread that made it; the world must outlive it, unchanged
// while chunks are in flight.
class GlobeStream {
public:
    GlobeStream(const globe::World& world, jobs::JobSystem& jobs, GlobeDetail detail = {});

    // Wants the surfaces round the eye, in metres from the planet's centre.
    void follow(DVec3 eye);
    // A brick of the world has changed: the chunks drawn from it are meshed again from the next
    // collect(). Nothing must be in flight while the world is changed; finish() first.
    void changed(globe::BrickSite brick);
    // Chunks still to mesh or let go, or changes not yet handed over.
    [[nodiscard]] bool in_flight() const noexcept;

    // What's changed since the last: chunks' meshes to show, each under its key, in place of any
    // there, and the keys of those to take away.
    struct Changes {
        std::vector<std::pair<std::uint64_t, std::shared_ptr<const terrain::BlockMesh>>> shown;
        std::vector<std::uint64_t> gone;

        [[nodiscard]] bool empty() const noexcept { return shown.empty() && gone.empty(); }
    };
    [[nodiscard]] Changes collect();
    // Waits for every chunk wanted first.
    [[nodiscard]] Changes finish();

    // Chunks shown, and those wanted round the eye, nearest first.
    [[nodiscard]] std::size_t chunks() const noexcept { return shown_.size(); }
    [[nodiscard]] std::span<const globe::Cell> wanted() const noexcept { return order_; }

private:
    // A chunk being meshed, against how those beside it were drawn when it started.
    struct Working {
        globe::Cell chunk;
        std::uint32_t beside = 0;
        std::unique_ptr<jobs::Async<globe::SurfaceMesh>> mesh;
    };

    void start_more();
    void take(std::size_t working);
    // Lets go of chunks no longer wanted whose ground those wanted now cover.
    void let_go();

    const globe::World* world_;
    jobs::JobSystem* jobs_;
    GlobeDetail detail_;
    std::optional<DVec3> around_;
    std::shared_ptr<const globe::Drawn> drawn_;
    std::vector<globe::Cell> order_;              // wanted, nearest first
    std::map<globe::Cell, std::uint32_t> wanted_; // and how those beside each are drawn
    std::vector<globe::Cell> queue_;              // to mesh, farthest first
    std::vector<Working> working_;
    std::map<globe::Cell, std::uint32_t> shown_; // as they were meshed against
    Changes changes_;
};

} // namespace hog::client
