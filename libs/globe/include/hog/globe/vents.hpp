#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/generator.hpp>

#include <cstdint>
#include <vector>

namespace hog::globe {

// A place where magma reaches the surface on dry land.
struct VentSite {
    DVec3 direction{0.0}; // unit, from the planet's centre
    double ground = 0.0;  // metres from the centre, at the surface there
    // Fixed per site, to give it its own cycle and size.
    std::uint64_t key = 0;
};

// Vents per square kilometre of land on a planet losing heat as Earth does; hotter planets have
// more, in proportion to the heat flow squared.
inline constexpr double vents_per_km2 = 1.0 / 3'000.0;

// Every vent on the planet, the same list each time: scattered over the surface by seed, and kept
// where the ground stands over the sea.
[[nodiscard]] std::vector<VentSite> vent_sites(const Generator& generator);

} // namespace hog::globe
