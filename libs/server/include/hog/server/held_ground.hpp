#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/volume.hpp>
#include <hog/globe/volume_archive.hpp>
#include <hog/globe/voxel.hpp>

#include <span>
#include <vector>

namespace hog::server {

// The tile columns of the world kept in memory: those near someone, read from the archive as they
// come near, and saved and let go once well away from everyone.
class HeldGround {
public:
    // Metres from a place its ground is held, and further still before it's let go, so walking
    // to and fro over a tile column's edge doesn't read and drop it each time.
    static constexpr double reach = 1000.0;
    static constexpr double slack = 250.0;

    HeldGround(globe::Volume& volume, globe::VolumeArchive& archive, globe::VolumeOrigin origin);

    // Whether any of `places` (from the planet's centre) is over another tile column than when
    // last kept, or their count differs.
    [[nodiscard]] bool moved(std::span<const DVec3> places) const;

    struct Changed {
        std::vector<globe::Site> loaded; // corners of tile columns, in order
        std::vector<globe::Site> dropped;
    };
    // Reads the tile columns within reach of `places` not held yet, and saves and lets go those
    // further than reach and slack from all of them, but those `pinned` (corners, in order).
    Result<Changed> keep(std::span<const DVec3> places, std::span<const globe::Site> pinned);

    // The corners of the tile columns the bricks are in, in order, each once.
    [[nodiscard]] static std::vector<globe::Site>
    corners_of(std::span<const globe::BrickSite> bricks);
    // A brick at the foot of each column of bricks in the tile column at `corner`: for whoever
    // looks at the ground from above.
    [[nodiscard]] static std::vector<globe::BrickSite> footprint(globe::Site corner);

private:
    [[nodiscard]] std::vector<globe::Site> under(std::span<const DVec3> places) const;
    [[nodiscard]] std::vector<globe::Site> near(std::span<const DVec3> places, double metres) const;

    globe::Volume* volume_;
    globe::VolumeArchive* archive_;
    globe::VolumeOrigin origin_;
    std::vector<globe::Site> under_; // the tile column under each place last kept, in order
};

} // namespace hog::server
