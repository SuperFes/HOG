#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>

#include <vector>

namespace hog::globe {

// How far from the eye a chunk is: from where the eye stands, `height` over the ground under the
// unit direction `up`, to the nearest of the chunk's ground, all taken on the planet's radius.
struct Eye {
    DVec3 up{0.0, 0.0, 1.0};
    double height = 0.0; // metres
};

[[nodiscard]] double distance_to(const Generator& generator, Eye eye, Cell chunk);

// Chunks tiling the ground within `reach` metres of the eye, each as coarse as keeps it `ratio`
// times its width away, nearest first. Chunks beside each other, across their sides, corners and
// the cube's edges, are at most one detail apart, as their seams are drawn between neighbours a
// detail apart. Precondition: ratio > 1.
[[nodiscard]] std::vector<Cell> chunks_around(const Generator& generator, Eye eye, double reach,
                                              double ratio);

} // namespace hog::globe
