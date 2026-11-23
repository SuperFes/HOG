#pragma once

#include <hog/client/held_pieces.hpp>
#include <hog/core/math.hpp>
#include <hog/flora/placement.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/water_bodies.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace hog::client {

// Where a line from `from` to `to` first goes from air into the ground, as the held pieces'
// surfaces have it, and which way the ground faces there. None where it doesn't, or where it
// passes anywhere nothing is held.
struct GroundHit {
    DVec3 at{0.0};
    DVec3 normal{0.0, 0.0, 1.0};
};
[[nodiscard]] std::optional<GroundHit> ground_along(const PiecesSnapshot& pieces, DVec3 from,
                                                    DVec3 to);

// The depth of the finest nodes the woods are held in.
[[nodiscard]] std::uint8_t woods_depth(const grid::Grid& grid) noexcept;

// No tree stands on ground facing further from up than this, as a cosine.
inline constexpr double steepest_stand = 0.6;

struct StandingTree {
    DVec3 at{0.0}; // where its trunk meets the ground
    flora::Planted tree;
};

// Whether the water a point's basin holds covers it at its highest in the year.
[[nodiscard]] bool under_water(const PiecesSnapshot& pieces, const grid::WaterBodies& bodies,
                               DVec3 point);

// The trees standing in a node of the woods' finest depth: those planted on its spots whose
// ground lies within it, each spot's line running down the planet's up through the node's middle,
// so ground in two nodes, one over the other, is planted once; none under water.
[[nodiscard]] std::vector<StandingTree> trees_in(const PiecesSnapshot& pieces,
                                                 const grid::WaterBodies& bodies, grid::Key node,
                                                 std::uint64_t seed,
                                                 const flora::Planting& planting);

} // namespace hog::client
