#pragma once

#include <hog/planet/cell_id.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/world.hpp>

namespace hog::terrain::testing {

inline const planet::PlanetConfig planet{.seed = 1, .ocean_fraction = 0.7, .radius = 115'000.0};

// One World for each test run: the seed's generators are costly to set up.
inline planet::World& shared_world() {
    static planet::World world{planet};
    return world;
}

// A level-6 patch over land, a few kilometres across, so its heights vary.
inline planet::CellId land_patch() {
    const auto& terrain = shared_world().geology().terrain();
    return planet::cell_at(terrain.archipelago().center(), 6);
}

} // namespace hog::terrain::testing
