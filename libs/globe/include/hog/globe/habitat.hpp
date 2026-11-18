#pragma once

#include <hog/core/math.hpp>
#include <hog/flora/habitat.hpp>
#include <hog/globe/generator.hpp>

namespace hog::globe {

// What a plant rooted at `direction` lives with, as the formed world has it there at the finest
// detail. Precondition: a formed world.
[[nodiscard]] flora::Habitat habitat(const Generator& generator, DVec3 direction);

} // namespace hog::globe
