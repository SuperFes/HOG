#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/physics/space.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/world.hpp>

#include <cstddef>
#include <map>
#include <span>

namespace hog::physics {

struct GroundSettings {
    double tile =
        50.0; // metres across a tile at most, on average; its vertices are 32 times closer
    double reach = 60.0; // metres around each point that the ground is kept out to, at least
};

// The planet's ground in a space, as drawn at its finest, kept only near the points asked for.
// The world mustn't change while it's held; the ground a change reaches is baked again only once
// let go of and taken back.
class Ground {
public:
    Ground(Space& space, const planet::World& world, const GroundSettings& settings = {});
    ~Ground();
    Ground(Ground&& other) noexcept;
    Ground& operator=(Ground&& other) noexcept;
    Ground(const Ground&) = delete;
    Ground& operator=(const Ground&) = delete;

    // Adds the ground within reach of each point, and lets go of tiles over twice as far from
    // every one of them.
    Result<> keep(std::span<const WorldPosition> around);

    [[nodiscard]] bool holds(planet::CellId tile) const { return tiles_.contains(tile); }
    [[nodiscard]] std::size_t size() const noexcept { return tiles_.size(); }
    // Of the tiles' cells.
    [[nodiscard]] int level() const noexcept { return level_; }

private:
    void clear() noexcept;

    Space* space_;
    const planet::World* world_;
    GroundSettings settings_;
    int level_;
    double across_; // metres across a tile, on average
    std::map<planet::CellId, BodyId> tiles_;
};

} // namespace hog::physics
