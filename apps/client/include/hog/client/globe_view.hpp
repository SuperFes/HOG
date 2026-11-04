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
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

// The voxel world a planet file describes: its [globe] table, with the world's seed, or Earth's
// knobs at the seed and radius given if it has none.
[[nodiscard]] Result<globe::Planet> globe_planet_in(const std::filesystem::path& file,
                                                    std::uint64_t seed, double radius);

// The voxel world drawn in place of the ground, nothing yet changed in it.
struct GlobeWorld {
    explicit GlobeWorld(const globe::Planet& planet,
                        const globe::Generator::Progress& progress = {})
        : generator{planet, progress}, store{generator.layers()}, world{generator, store} {}
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

// A chunk's ground as blocks, laid from the chunk's own origin, each triangle wearing its
// corners' materials; the liquid's surfaces are left out, as the sea is drawn over the ground
// and still water as globe_still_water has it.
[[nodiscard]] terrain::BlockMesh globe_blocks(const globe::SurfaceMesh& mesh);
// A chunk's liquid surfaces standing clear of the sea's level, for the sea to draw as still
// water; those within a voxel of its level are the sea's own, drawn over the ground.
[[nodiscard]] terrain::BlockMesh globe_still_water(const globe::SurfaceMesh& mesh,
                                                   std::optional<double> sea_level,
                                                   double thickness);

// How the globe's detail falls off: each chunk as coarse as keeps it `ratio` of its widths from
// the eye, out to the horizon and no less than `least` round it.
struct GlobeDetail {
    double ratio = 3.0;
    double least = 1'000.0; // metres
};

// The chunks are sought anew once the eye has gone this far, or a twentieth of its height.
inline constexpr double globe_relay = 16.0; // metres

// What lies under the ground is meshed only for chunks this near the eye, and only as far down and
// up as the eye can see there: past that, rock hides it.
inline constexpr double globe_sight = 96.0; // metres

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

    // What's changed since the last: chunks' meshes to show, the ground's and the still water's,
    // each under its key, in place of any there, and the keys of those to take away.
    struct Shown {
        std::uint64_t key = 0;
        std::shared_ptr<const terrain::BlockMesh> ground;
        std::shared_ptr<const terrain::BlockMesh> water;
    };
    struct Changes {
        std::vector<Shown> shown;
        std::vector<std::uint64_t> gone;

        [[nodiscard]] bool empty() const noexcept { return shown.empty() && gone.empty(); }
    };
    [[nodiscard]] Changes collect();
    // Waits for every chunk wanted first.
    [[nodiscard]] Changes finish();

    // Chunks shown, and those wanted round the eye, nearest first.
    [[nodiscard]] std::size_t chunks() const noexcept { return shown_.size(); }
    [[nodiscard]] std::span<const globe::Cell> wanted() const noexcept { return order_; }
    // The chunks drawn at full detail at any time since the start, which is the ground near where
    // the eye has been.
    [[nodiscard]] const std::set<globe::Cell>& finest() const noexcept { return finest_; }

private:
    // A chunk being meshed, against how those beside it were drawn and the band of radii its
    // sight was made for, when it started.
    struct Working {
        globe::Cell chunk;
        std::uint64_t against = 0;
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
    std::map<globe::Cell, std::uint64_t> wanted_; // and what each is meshed against
    std::vector<globe::Cell> queue_;              // to mesh, farthest first
    std::vector<Working> working_;
    std::map<globe::Cell, std::uint64_t> shown_; // as they were meshed against
    std::set<globe::Cell> finest_;               // ever drawn at full detail
    Changes changes_;
};

} // namespace hog::client
