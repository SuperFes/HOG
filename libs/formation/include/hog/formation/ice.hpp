#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/formation/plates.hpp>
#include <hog/formation/rivers.hpp>
#include <hog/formation/sphere.hpp>

#include <vector>

namespace hog::formation {

// Ice the climate keeps on the land: where more snow falls through the year than summers melt,
// gathered down the land as glaciers and ice sheets as far as it lasts against the melt, and as
// thick as ice stands before it flows (Nye's perfectly plastic ice). Worked out first for the
// coldest times the planet has known, whose ice carved the land, then for the climate as it is,
// whose ice lies on what was carved.
struct Ice {
    std::vector<double> thickness; // metres of ice lying now
    std::vector<double> surface;   // metres, the ice's top where it lies, else the rock
    std::vector<double> balance;   // metres of water a year snow gives the ice over what melts
    std::vector<double> flux;      // cubic metres of water a year passing on as ice
    std::vector<double> coldest;   // metres of ice that lay in the coldest times
    std::vector<double> carved;    // metres of rock that ice carved away
    std::vector<double> sea_ice;   // share of the year the open sea lies frozen
    double calving = 0.0;          // cubic metres of water a year ice breaks off into the sea
};

// Over a year whose temperature swings `swing` either side of `mean` and back, how many
// degree-days it spends above 0, and what share of it is spent below.
[[nodiscard]] double degree_days(double mean, double swing);
[[nodiscard]] double cold_share(double mean, double swing);

// How far a place's year swings either side of its mean, from its latitude and the tilt.
[[nodiscard]] double seasonal_swing(double latitude, bool sea, const Body& body);

// Metres of plastic ice over `bed`, `length` metres up from where the ice's top stands at `below`.
[[nodiscard]] double ice_thickness(double bed, double below, double length, const Body& body);

// How far a bed may dip under the bed `length` metres down-ice of it, where the ice there is
// `ice_below` thick, before the bed rises towards the margin more steeply than the overdeepening
// allows against the plastic ice's falling top.
[[nodiscard]] double deepest_dip(double ice_below, double length, const Body& body);

[[nodiscard]] Ice ice_on(const Sphere& sphere, const Crust& crust, const Climate& climate,
                         const Rivers& rivers, const Body& body);

// The land as water finds it after ice: rock lowered where ice carved it, ice lying on top where it
// lies now, and the sea let into troughs carved below its level.
[[nodiscard]] Rivers ice_shaped(const Sphere& sphere, const Rivers& rivers, const Ice& ice);

} // namespace hog::formation
