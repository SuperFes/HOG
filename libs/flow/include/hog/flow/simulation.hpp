#pragma once

#include <hog/flow/region.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/world.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace hog::flow {

// The flow run where the world is touched: an edit wakes the bricks about it, liquid takes up
// the bricks it reaches, and bricks still for a while are let go, so what's at rest costs
// nothing. Each step writes the bricks it changed to the world, which is how anyone hears of it.
class Simulation {
public:
    // Steps a brick goes unchanged before it's let go.
    static constexpr int rest = 60;
    // Bricks taken up in one step, at most; the rest wait their turn.
    static constexpr std::size_t most_taken = 64;

    explicit Simulation(globe::World& world);

    // Wakes the brick and the six about it.
    void touched(globe::BrickSite site);

    struct Progress {
        std::int64_t changed = 0;
        std::int64_t eroded = 0;
        std::int64_t slumped = 0;
        std::size_t held = 0;
        std::vector<globe::BrickSite> written;
    };
    // Runs the liquid `seconds` on.
    Progress step(double seconds);

    [[nodiscard]] const Region& region() const noexcept { return region_; }

    // What the flow has left of itself in a column: how much water has stood there, in metres
    // of depth times seconds, and how much has run through, in voxels' worth. Vegetation reads
    // these to grow thick by water and thin away from it.
    struct Moisture {
        double stood = 0.0;
        double ran = 0.0;
    };
    [[nodiscard]] Moisture moisture(std::uint8_t face, std::uint32_t i, std::uint32_t j) const;

private:
    void take(globe::BrickSite site);

    globe::World* world_;
    Region region_;
    std::unordered_map<std::uint64_t, int> still_; // steps each brick held has gone unchanged
    std::unordered_map<std::uint64_t, Moisture> moisture_;
};

} // namespace hog::flow
