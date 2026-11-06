#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/world.hpp>
#include <hog/terrain/block_mesh.hpp>

namespace hog::client {

// The inside of the ground, seen from within it: the face of a ball of `radius` metres about
// `centre`, drawn from the voxels it passes through, each as the material it's made of, facing in.
// Where the ball is out in open space, a cave, it has no face.
[[nodiscard]] terrain::BlockMesh cutaway_shell(const globe::World& world, WorldPosition centre,
                                               double radius);

} // namespace hog::client
