#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/ice.hpp>
#include <hog/formation/lakes.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sediment.hpp>
#include <hog/formation/sphere.hpp>

#include <cstdint>
#include <vector>

namespace hog::formation {

// What soil forms from.
enum class Parent : std::uint8_t {
    None,     // under ice, standing water or salt: no soil forms
    Rock,     // the rock itself
    Till,     // what ice laid down
    Alluvium, // what rivers and lakes laid down
};

// How deep soil has grown over the time it has had (Heimsath's soil production: what it forms from
// turns to soil fastest when bare and slower under deeper soil, while the ground wears away).
struct Soil {
    std::vector<double> depth;  // metres of soil
    std::vector<double> loose;  // metres of loose till or alluvium under it, not yet soil
    std::vector<double> making; // metres a year its bare parent turns to soil there
    std::vector<double> years;  // years it has had to grow
    std::vector<Parent> parent;
};

// Metres a year bare rock turns to soil: rotting faster where it's warm and wet, shattered by frost
// a little under freezing, neither where it's frozen through.
[[nodiscard]] double soil_making(double celsius, double rain, const Body& body);
// The depth at which soil made under it matches `wear`; none where wear outpaces making.
[[nodiscard]] double balanced_depth(double making, double wear, const Body& body);
// The depth soil grows to from bare ground over `years`, tending to the balanced depth.
[[nodiscard]] double grown_depth(double making, double wear, double years, const Body& body);

// Soil on rock alone, as though nothing had been laid down and no ice had ever lain.
[[nodiscard]] Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate,
                           const Body& body);
// Soil on rock or on what ice and rivers laid over it, none under present ice or standing water.
[[nodiscard]] Soil soil_on(const Sphere& sphere, const Rivers& rivers, const Climate& climate,
                           const Ice& ice, const Lakes& lakes, const Sediment& sediment,
                           const Body& body);

} // namespace hog::formation
