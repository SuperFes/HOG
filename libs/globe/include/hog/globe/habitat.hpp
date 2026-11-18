#pragma once

#include <hog/core/math.hpp>
#include <hog/flora/habitat.hpp>
#include <hog/flora/woods.hpp>
#include <hog/globe/generator.hpp>

namespace hog::globe {

// What a plant rooted at `direction` lives with, as the formed world has it there with detail no
// finer than `lod` metres, the finest where none's given. Precondition: a formed world.
[[nodiscard]] flora::Habitat habitat(const Generator& generator, DVec3 direction);
[[nodiscard]] flora::Habitat habitat(const Generator& generator, DVec3 direction, double lod);

// How the stands at `direction` lean off what the habitat alone grows, by the world's own noise:
// kinds wandering into groves a few hundred metres across, the odd outlying grove, and glades.
// Detail finer than `lod` metres is left out, so coarse samples are smooth versions of fine ones.
[[nodiscard]] flora::Lean lean(const Generator& generator, DVec3 direction, double lod);

// The woods at `direction`: its habitat's, leaned by its stands. Precondition: a formed world.
[[nodiscard]] flora::Woods woods(const Generator& generator, DVec3 direction, double lod);

} // namespace hog::globe
