#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/lakes.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sphere.hpp>

#include <span>
#include <vector>

namespace hog::formation {

// Where the rock worn off the land ends up: carried down the rivers as far as they can carry it,
// dropped where they slow (fans at mountain fronts, floodplains, deltas at the coast), and kept by
// hollows until they're full.
struct Sediment {
    std::vector<double> flux;       // cubic metres a year carried on past each point
    std::vector<double> settling;   // cubic metres a year settling at each point, land or sea
    std::vector<double> alluvium;   // metres laid down on land: fans, floodplains, filled hollows
    std::vector<double> delta;      // metres laid on the sea floor, out from river mouths no
                                    // higher than the waterline
    std::vector<double> lake_depth; // lakes' water depth once what settles in them is taken off
};

// Cubic metres a year a river can carry: more water and steeper ground carry more.
[[nodiscard]] double carrying_capacity(double discharge, double slope, const Body& body);

[[nodiscard]] Sediment sediment_on(const Sphere& sphere, const Rivers& rivers, const Lakes& lakes,
                                   const Body& body);
// As above, with `brought` cubic metres more handed to the rivers at each point over the land's
// time, such as ice's outwash.
[[nodiscard]] Sediment sediment_on(const Sphere& sphere, const Rivers& rivers, const Lakes& lakes,
                                   std::span<const double> brought, const Body& body);

} // namespace hog::formation
