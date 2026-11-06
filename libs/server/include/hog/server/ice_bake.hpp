#pragma once

#include <hog/flow/simulation.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/server/ice_climate.hpp>

#include <cstdint>
#include <vector>

namespace hog::server {

// Ice thickness over a square of ground, on a grid coarser than the voxel columns: a glacier is
// kilometres long, and its flow needs nothing finer than tens of metres.
struct IceMap {
    globe::Cell corner;       // the first voxel column the map covers
    std::uint32_t stride = 1; // voxel columns to a node
    std::uint32_t side = 0;   // nodes along each side
    std::vector<double> ice;  // metres at each node, row by row along j

    [[nodiscard]] std::uint32_t columns() const noexcept { return side * stride; }
    // Metres of ice over the voxel column `i` and `j` columns along from the corner, read between
    // the nodes about it; none outside the map.
    [[nodiscard]] double at(std::uint32_t i, std::uint32_t j) const noexcept;
};

// Settles `years` of the climate's snow and melt on the ground of `side` by `side` nodes of
// `stride` voxel columns from `corner`, with ice flowing downhill and off the map's rim. Nothing
// is placed: the ice is whatever the snowline, the slopes and the flow leave.
[[nodiscard]] IceMap bake_ice_map(const globe::Generator& generator, globe::Cell corner,
                                  std::uint32_t side, std::uint32_t stride,
                                  const ClimateAt& climate, int years);

// The voxel columns the map has at least `least` metres of ice over, with their ice.
[[nodiscard]] std::vector<flow::Simulation::Laid> laid_from(const IceMap& map, double least = 0.25);

} // namespace hog::server
