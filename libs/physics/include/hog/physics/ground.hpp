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
#include <set>
#include <span>

namespace hog::physics {

struct GroundSettings {
    double reach = 60.0; // metres around each point that the ground is kept out to, at least
};

// The voxel world's ground in a space, its voxels' own boxes, kept only near the points asked for,
// a tile at a time: the finest the world holds over each place, at its own level. A change lets go
// of the tiles it touches, to be meshed again as they're kept.
class Ground {
public:
    Ground(Space& space, globe::World world, const GroundSettings& settings = {});
    ~Ground();
    Ground(Ground&& other) noexcept;
    Ground& operator=(Ground&& other) noexcept;
    Ground(const Ground&) = delete;
    Ground& operator=(const Ground&) = delete;

    // Adds the ground within reach of each point, and lets go of tiles over twice as far from
    // every one of them.
    Result<> keep(std::span<const WorldPosition> around);
    // Lets go of the tiles drawn from `brick`, or reading it at their edges.
    void changed(globe::BrickSite brick);
    // Lets go of the tiles that share space with the tile at `key`, or meet it.
    void changed(globe::TileKey key);

    // Whether the tile at `key`, or one kept over it, is kept, with ground in it or none.
    [[nodiscard]] bool holds(globe::TileKey key) const;
    // The tiles kept with ground in the space.
    [[nodiscard]] std::size_t size() const noexcept;

private:
    void let_go(globe::TileKey key) noexcept;
    void clear() noexcept;
    // The tiles kept for the finest ones within `reach` of each point.
    [[nodiscard]] std::set<globe::TileKey> near(std::span<const WorldPosition> around,
                                                double reach) const;

    Space* space_;
    globe::World world_;
    GroundSettings settings_;
    // Each tile kept: its body, none where it has no ground.
    std::map<globe::TileKey, std::optional<BodyId>> tiles_;
};

} // namespace hog::physics
