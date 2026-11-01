#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/world.hpp>
#include <hog/physics/space.hpp>

#include <cstddef>
#include <map>
#include <span>

namespace hog::physics {

struct GroundSettings {
    double reach = 60.0; // metres around each point that the ground is kept out to, at least
};

// The voxel world's ground in a space, as drawn at its finest, kept only near the points asked
// for, a chunk at a time. The world mustn't change while a chunk is meshed; a brick changed
// lets go of the chunks drawn from it, to be meshed again as they're kept.
class Ground {
public:
    Ground(Space& space, globe::World world, const GroundSettings& settings = {});
    ~Ground();
    Ground(Ground&& other) noexcept;
    Ground& operator=(Ground&& other) noexcept;
    Ground(const Ground&) = delete;
    Ground& operator=(const Ground&) = delete;

    // Adds the ground within reach of each point, and lets go of chunks over twice as far from
    // every one of them.
    Result<> keep(std::span<const WorldPosition> around);
    // Lets go of the chunks drawn from `brick`.
    void changed(globe::BrickSite brick);

    [[nodiscard]] bool holds(globe::Cell chunk) const { return chunks_.contains(chunk); }
    [[nodiscard]] std::size_t size() const noexcept { return chunks_.size(); }

private:
    void clear() noexcept;

    Space* space_;
    globe::World world_;
    GroundSettings settings_;
    std::map<globe::Cell, BodyId> chunks_;
};

} // namespace hog::physics
