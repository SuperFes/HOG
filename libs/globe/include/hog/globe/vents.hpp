#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/generator.hpp>

#include <hog/globe/cell.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace hog::globe {

// A place where magma reaches the surface on dry land.
struct VentSite {
    DVec3 direction{0.0}; // unit, from the planet's centre
    double ground = 0.0;  // metres from the centre, at the surface there or a cave's floor
    bool pool = false;    // a pool on a cavern's floor, rather than a volcano's throat
    // Fixed per site, to give it its own cycle and size.
    std::uint64_t key = 0;
};

// Vents per square kilometre of land on a planet losing heat as Earth does; hotter planets have
// more, in proportion to the heat flow squared.
inline constexpr double vents_per_km2 = 1.0 / 3'000.0;

// A volcano: a cone with a crater, its outline warped by a few harmonics so no two are round.
struct Volcano {
    DVec3 centre{0.0};   // unit, from the planet's centre
    double radius = 0.0; // metres, the mean base
    double height = 0.0; // metres over the ground, before the crater
    double crater = 0.0; // share of the radius
    double depth = 0.0;  // metres, of the crater below the cone's top
    // The outline's warp: radius times 1 + sum of amplitude * cos(k * bearing + phase), k = 2, 3, 5.
    std::array<double, 3> amplitude{};
    std::array<double, 3> phase{};
    std::uint64_t key = 0;
};

// The volcano whose cell it is, if it has one: the same for the same planet.
[[nodiscard]] std::optional<Volcano> volcano_in(const Planet& planet, Cell cell);

// Cavern pools tried per square kilometre of land, on a planet losing heat as Earth does; most
// find no basalt cave to lie in.
inline constexpr double pools_per_km2 = 1.0 / 400.0;

// Every vent on the planet, the same list each time: scattered over the surface by seed, and kept
// where the ground stands over the sea.
[[nodiscard]] std::vector<VentSite> vent_sites(const Generator& generator);

} // namespace hog::globe
