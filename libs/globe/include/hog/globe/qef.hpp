#pragma once

#include <hog/core/math.hpp>

#include <span>

namespace hog::globe {

// The point nearest, in least squares, to the planes through each point square to its unit normal,
// as dual contouring places a cube's vertex where the surface's tangent planes meet. Along
// directions the planes barely pin down, as where they're nearly parallel, it stays at the points'
// mean rather than running off to where nearly parallel planes happen to cross.
// Precondition: as many normals as points, at least one.
[[nodiscard]] DVec3 nearest_to_planes(std::span<const DVec3> points,
                                      std::span<const DVec3> normals);

} // namespace hog::globe
