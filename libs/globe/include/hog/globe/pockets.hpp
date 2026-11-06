#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/world.hpp>

#include <vector>

namespace hog::globe {

// A pocket of open air under the ground, as far as the ground there stands over it.
struct Pocket {
    DVec3 at{0.0};       // metres from the planet's centre, the middle of its open space
    double across = 0.0; // metres, about as wide as it runs along the ground
    double height = 0.0; // metres, the most open space up and down in it
};

// The air pockets under matter within `range` metres of `eye`, nearest first, found by reading
// down the voxels of columns spaced `spacing` metres apart about it: those whose open space is
// wider than that are found. Caves open to the ground and holes dug alike.
[[nodiscard]] std::vector<Pocket> find_pockets(const World& world, DVec3 eye, double range,
                                               double spacing);

} // namespace hog::globe
