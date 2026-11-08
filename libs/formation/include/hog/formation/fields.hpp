#pragma once

#include <hog/formation/sphere.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::formation {

// Smooth noise over the sphere's points, about -1 to 1: random values on coarser spheres, from
// `coarsest` to `finest` splits, each finer one half as strong, read between their points.
[[nodiscard]] std::vector<double> sphere_noise(const Sphere& on, std::uint64_t seed, int coarsest,
                                               int finest);

// Where on the sphere something is, and the point it lies at or next to.
struct Source {
    DVec3 where{};
    std::uint32_t point = 0;
};

// For each point, the nearest source and how far it is, in radians: spread from the sources over
// the neighbours, keeping true distances rather than path lengths, so there's no grid in it.
struct Nearest {
    std::vector<std::uint32_t> source; // none where there were no sources
    std::vector<double> distance;
    static constexpr std::uint32_t none = 0xFFFF'FFFFU;
};
[[nodiscard]] Nearest nearest_sources(const Sphere& sphere, std::span<const Source> sources);

} // namespace hog::formation
