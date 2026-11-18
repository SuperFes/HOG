#pragma once

namespace hog::flora {

// What a plant rooted at a place lives with.
struct Habitat {
    double temperature = 0.0; // °C, the yearly mean at the ground's own height
    double swing = 0.0;       // °C the warmest month stands over the mean, the coldest under
    double rain = 0.0;        // metres of water falling a year
    // Metres the ground stands over the floor of the valley it lies in, or the lake it drains
    // to: how far down the water under it lies.
    double over_drainage = 0.0;
    // Cubic metres a second running down the valleys about at their mean: what keeps the water
    // under them near the ground, where the rain alone wouldn't.
    double flow = 0.0;
    double soil = 0.0;  // metres of soil, litter and peat over the rock and what's laid on it
    double bare = 0.0;  // share of the ground standing as bare rock, 0 to 1
    double slope = 0.0; // rise over run
    double ice = 0.0;   // metres of ice lying the year round
    // Metres of open water standing over the ground at its lowest in the year, and whether it's
    // the sea's.
    double water = 0.0;
    bool sea = false;
};

} // namespace hog::flora
