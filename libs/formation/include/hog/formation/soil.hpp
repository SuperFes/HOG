#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sphere.hpp>

#include <vector>

namespace hog::formation {

// How deep soil lies where rock turning to soil has come to balance the ground wearing away
// (Heimsath's soil production: rock weathers fastest when bare and slower under deeper soil).
struct Soil {
    std::vector<double> depth;  // metres of soil over rock
    std::vector<double> making; // metres a year bare rock turns to soil there
};

// Metres a year bare rock turns to soil: rotting faster where it's warm and wet, shattered by frost
// a little under freezing, neither where it's frozen through.
[[nodiscard]] double soil_making(double celsius, double rain, const Body& body);
// The depth at which soil made under it matches `wear`; none where wear outpaces making.
[[nodiscard]] double balanced_depth(double making, double wear, const Body& body);

[[nodiscard]] Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate,
                           const Body& body);

} // namespace hog::formation
