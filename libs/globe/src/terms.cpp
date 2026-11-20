#include "terms.hpp"

#include <hog/core/hash.hpp>
#include <hog/globe/layers.hpp>

#include <algorithm>
#include <cmath>

namespace hog::globe::terms {

std::uint64_t Context::seed(Term term) const noexcept {
    return hash_values(planet->body.seed, static_cast<std::uint64_t>(term));
}

DVec3 tangential(DVec3 gradient, DVec3 direction) noexcept {
    return gradient - (glm::dot(gradient, direction) * direction);
}

double cell_lod(const Planet& planet, int level) noexcept {
    const int voxels = level_about(planet.body.radius, planet.voxel);
    return std::max(std::ldexp(planet.voxel, voxels - level), 2.0 * planet.voxel);
}

int cell_level(const Planet& planet, double lod) noexcept {
    const int voxels = level_about(planet.body.radius, planet.voxel);
    int level = voxels;
    while (level > 0 && std::ldexp(planet.voxel, voxels - level + 1) <= lod) {
        --level;
    }
    return level;
}

double smoothstep(double x) noexcept {
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

double smoothstep_slope(double x) noexcept {
    const double t = std::clamp(x, 0.0, 1.0);
    return 6.0 * t * (1.0 - t);
}

} // namespace hog::globe::terms
