#pragma once

#include <hog/globe/globe.hpp>

#include <cstdint>

namespace hog::globe {

// How a planet gathers itself out of the disc: bodies of every size fall on it, each piling its
// metal and rock where it lands and heating them by the energy of its fall, until the planet has
// grown to its radius. Piles higher than their rock bears slump away as they land.
struct Accretion {
    std::uint64_t seed = 1;
    double radius = 115'000.0; // metres, as grown
    double gravity = 9.81;     // m/s² at the surface, as grown
    int level = 5;             // the columns it's gathered in
    // Shares by mass of what fell: chondrite's metal; the rest is rock holding its water and gas.
    double iron = 0.32;
    // What fell was as cold as the disc; its fall heats it by the share of its energy kept, more as
    // the planet grows and pulls harder, and short-lived isotopes heat everything alike.
    double disc_temperature = 250.0; // kelvin
    double heat_kept = 0.5;
    double heat_capacity = 1'000.0; // J/kg/K
    double decay_heat = 600.0;      // kelvin
    // Footprints of the bodies, as arcs of the sphere: most are the smallest, a few vast. A body
    // piles up a fifth as deep as its footprint is wide.
    double largest = 0.35; // radians
    double size_spread = 2.5;
};

// Hexels are as thick as the columns at the surface are wide, with room above for what piles high.
[[nodiscard]] Shells accretion_shells(const Accretion& setup);

[[nodiscard]] Globe accrete(const Accretion& setup);

} // namespace hog::globe
