#pragma once

#include <hog/core/math.hpp>
#include <hog/server/spatial_index.hpp>

#include <flecs.h>

namespace hog::server {

// Where an entity is in the world.
struct Position {
    WorldPosition at;
};

// Keeps `index` in step with every entity's Position: placed or moved at once by set(), moved
// after the update phase of each progress() when systems write positions in place, and dropped
// when the position or the entity goes. The index must outlive the world.
void track_positions(flecs::world& world, SpatialIndex& index);

} // namespace hog::server
