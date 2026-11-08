#pragma once

#include <hog/formation/bedrock.hpp>
#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/ice.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sphere.hpp>

#include <vector>

namespace hog::formation {

// How likely each landform is at a point, 0 to 1: not the shapes themselves, which are too small to
// hold here, but where what makes them comes together, for shapes made finer to follow.
struct Landforms {
    std::vector<double> canyons;     // a river cutting fast into strong rock, walls kept by drought
    std::vector<double> mesas;       // flat-lying rock under a hard cap, cut into tables and buttes
    std::vector<double> escarpments; // the edges of hard beds standing over soft, in steps
    std::vector<double> badlands;    // soft uniform beds gullied bare where it's dry
    std::vector<double> karst;       // rock dissolving: sinkholes, towers, caves
    std::vector<double> sea_cliffs;  // the coast cut back into strong rock standing over the sea
    std::vector<double> glacial;     // walls, cirques and horns where ice carved
    std::vector<double> fault_scarps; // ground broken and lifted along a plate boundary
    std::vector<double> volcanoes;    // cones and craters where lava still comes up
    std::vector<double> scree;        // frost-shattered rock heaped under steep ground
    std::vector<double> strength;     // how well the rock at the surface stands steep, 0 to 1
    std::vector<double> slope;        // rise over run to the steepest neighbour
};

[[nodiscard]] Landforms landforms_on(const Sphere& sphere, const Crust& crust,
                                     const Bedrock& bedrock, const Outcrop& outcrop,
                                     const Climate& climate, const Rivers& rivers, const Ice& ice,
                                     const Body& body);

} // namespace hog::formation
