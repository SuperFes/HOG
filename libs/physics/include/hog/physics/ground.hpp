#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/world.hpp>
#include <hog/physics/space.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>

namespace hog::physics {

struct GroundSettings {
    double reach = 60.0; // metres around each point that the ground is kept out to, at least
};

// The voxel world's ground in a space, its voxels' own boxes, kept only near the points asked for,
// a tile column at a time as the world holds it at its own level. A brick changed lets go of the
// tile columns drawn from it, to be meshed again as they're kept.
class Ground {
public:
    Ground(Space& space, globe::World world, const GroundSettings& settings = {});
    ~Ground();
    Ground(Ground&& other) noexcept;
    Ground& operator=(Ground&& other) noexcept;
    Ground(const Ground&) = delete;
    Ground& operator=(const Ground&) = delete;

    // Adds the ground within reach of each point, and lets go of tile columns over twice as far
    // from every one of them.
    Result<> keep(std::span<const WorldPosition> around);
    // Lets go of the tile columns drawn from `brick`: its own, and the one beside it where it
    // lies on its edge.
    void changed(globe::BrickSite brick);

    // Whether the tile column at `corner` is kept, with ground in it or none.
    [[nodiscard]] bool holds(globe::Site corner) const { return columns_.contains(corner); }
    // The tile columns kept with ground in the space.
    [[nodiscard]] std::size_t size() const noexcept;

private:
    void let_go(globe::Site corner) noexcept;
    void clear() noexcept;

    Space* space_;
    globe::World world_;
    GroundSettings settings_;
    // Each tile column kept, by its corner: its body, none where it has no ground.
    std::map<globe::Site, std::optional<BodyId>> columns_;
};

} // namespace hog::physics
