#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/detail.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/surface_mesh.hpp>
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

namespace hog::chunks {

// Metres the ground's voxels stand over the planet's radius under a point.
[[nodiscard]] double globe_ground(const globe::World& world, DVec3 at);

// A chunk's ground as blocks, laid from the chunk's own origin, each triangle wearing its
// corners' materials; the liquid's surfaces are left out, as the sea is drawn over the ground
// and still water as globe_still_water has it.
[[nodiscard]] terrain::BlockMesh globe_blocks(const globe::SurfaceMesh& mesh);
// A chunk's liquid surfaces, for the sea to draw over the ground as still water: those within a
// voxel of the sea's level are the sea's own, marked so by globe_sea_corner, and drawn with its
// waves as it lies there; the rest stand clear of it.
inline constexpr std::uint8_t globe_sea_corner = 1;
[[nodiscard]] terrain::BlockMesh globe_still_water(const globe::SurfaceMesh& mesh,
                                                   std::optional<double> sea_level,
                                                   double thickness);

// How the globe's detail falls off: each chunk as coarse as keeps it `ratio` of its widths from
// the eye, out to the horizon; past `least` metres from the eye, a `slope` above 1 has it fall off
// faster still, so the ground a long way off is coarser.
struct GlobeDetail {
    double ratio = 3.0;
    double least = 1'000.0; // metres
    double slope = 1.0;
};

// The chunks are sought anew once the eye has gone this far, or a twentieth of its height.
inline constexpr double globe_relay = 16.0; // metres
// Chunks within this cosine of the look direction (about 60 degrees) are in view, and the look
// turning past the second cosine (about 15 degrees) has them listed again.
inline constexpr double globe_view_cos = 0.5;
inline constexpr double globe_turn_cos = 0.966;

// What lies under the ground is meshed only for chunks this near the eye, and only as far down and
// up as the eye can see there: past that, rock hides it.
inline constexpr double globe_sight = 96.0; // metres

// The chunk a key names, as globe_key made it.
[[nodiscard]] globe::Cell globe_cell(std::uint64_t key);

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

    // Wants the surfaces round the eye, in metres from the planet's centre. Looking along
    // `forward` (zero if unknown), those in view are meshed and listed first, nearest first.
    void follow(DVec3 eye, DVec3 forward = {});
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
    // Waits for the chunks being meshed, and no more are begun until the next collect(), so the
    // world may be changed; what they made is handed over by that collect().
    void settle();
    // While held, no more chunks are begun, and those under way finish: so the world may be
    // changed once none are meshing.
    void hold(bool on) noexcept { held_ = on; }
    // Forgets every chunk shown, so those wanted are meshed again and shown as if for the first
    // time, once the eye next follows. None may be being meshed.
    void forget();
    // Wants the detail to fall off as given from now, once the eye next follows.
    void set_detail(GlobeDetail detail) noexcept {
        detail_ = detail;
        around_.reset();
    }
    // Chunks being meshed now.
    [[nodiscard]] std::size_t meshing() const noexcept { return working_.size(); }

    // Metres along the ground from the point under the eye within which every chunk wanted is
    // shown; none before the eye's followed.
    [[nodiscard]] double covered() const;

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
    bool held_ = false;
    std::optional<DVec3> around_;
    double reach_ = 0.0; // metres along the ground the chunks wanted reach
    DVec3 looking_{0.0};
    std::shared_ptr<const globe::Drawn> drawn_;
    std::vector<globe::Cell> order_;              // wanted, nearest first
    std::map<globe::Cell, std::uint64_t> wanted_; // and what each is meshed against
    std::vector<globe::Cell> queue_;              // to mesh, farthest first
    std::vector<Working> working_;
    std::map<globe::Cell, std::uint64_t> shown_; // as they were meshed against
    std::set<globe::Cell> finest_;               // ever drawn at full detail
    Changes changes_;
};

} // namespace hog::chunks
