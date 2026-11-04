#pragma once

#include <hog/globe/planet.hpp>

// These tests were tuned on 0.5 m voxels, finer than the planets' default.
[[nodiscard]] inline hog::globe::Planet fine(hog::globe::Planet planet) {
    planet.voxel = 0.5;
    return planet;
}
