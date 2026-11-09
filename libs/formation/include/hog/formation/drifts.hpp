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

// What the wind has carried off loose ground lying bare and dry, and laid down again: sand blown a
// little way on into dune fields and sand sheets, banked where plants catch it, wet ground holds
// it or the land rises in its way; silt carried far off as loess, settling where plants trap it.
// Sources are dry, bare river-laid ground and weathered rock, ground ice left bare in the coldest
// times, and beach sand blown ashore.
struct Drifts {
    std::vector<double> deflated; // metres of loose ground the wind took away
    std::vector<double> sand;     // metres of wind-laid sand
    std::vector<double> loess;    // metres of wind-laid silt
    std::vector<double> dunes;    // 0 to 1: how likely dunes stand, sand deep on bare, dry ground
};

// How dry the ground lies for the wind to lift it, 0 wet to 1 dry, from the rain against what
// open water would lose to the air.
[[nodiscard]] double dryness(double celsius, double rain, const Body& body);

// What of a drift lies as a layer of its own: none where it's thin enough for plants and burrows
// to mix it into the soil.
[[nodiscard]] double layered(double drift, const Body& body);

// Where each point's wind blows on to: the neighbour most nearly downwind; itself where none is.
[[nodiscard]] std::vector<std::uint32_t> downwind(const Sphere& sphere, const Climate& climate);

[[nodiscard]] Drifts drifts_on(const Sphere& sphere, const Climate& climate, const Rivers& rivers,
                               const Ice& ice, const Lakes& lakes, const Sediment& sediment,
                               const Body& body);

} // namespace hog::formation
