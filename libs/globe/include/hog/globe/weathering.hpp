#pragma once

#include <hog/globe/material.hpp>

namespace hog::globe {

// The loose ground rock weathers into where it lies, layered down from the surface: topsoil, a
// subsoil the water has washed fines into, then rock broken where it lies, down to the rock.
struct Horizons {
    double topsoil = 0.0; // metres down to the bottom of each layer
    double subsoil = 0.0;
    double depth = 0.0; // to the rock
    Material top = Material::Soil;
    Material under = Material::Clay;
    Material broken = Material::Gravel;

    // The layer `below` metres down; Air past the depth.
    [[nodiscard]] Material at(double below) const noexcept;
};

// The climate a place has weathered under.
struct Climate {
    double kelvin = 288.0; // the year's mean at the ground
    double rain = 0.8;     // metres a year
    bool liquid = true;    // whether water runs on the world at all
};

// What `rock` weathers into under `climate`, `depth` metres deep where the weather is
// temperate and the rock middling. Water and warmth rot rock to clay and feed the topsoil, about
// doubling for every ten kelvin; frost alone only breaks it; with no water at all it's only
// shattered by impacts into regolith. Ground that isn't rock doesn't weather into anything.
[[nodiscard]] Horizons horizons(Material rock, const Climate& climate, double depth);

} // namespace hog::globe
