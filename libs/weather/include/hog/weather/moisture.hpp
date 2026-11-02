#pragma once

#include <hog/globe/planet.hpp>
#include <hog/weather/field.hpp>
#include <hog/weather/stencil.hpp>

namespace hog::weather {

// A parcel of air: what it holds, per kilogram of dry air.
struct Parcel {
    double temperature = 0.0; // kelvin
    double vapour = 0.0;
    double cloud = 0.0;
};

// Brings a parcel at `pressure` to balance with its cloud: vapour over what the air can hold
// condenses, warming the air by the heat it gives up, and cloud in air that can hold more
// evaporates, cooling it. Water, and the heat of the air plus that of its vapour, are kept.
void saturate(Parcel& parcel, globe::Volatile volatiles, double latent_heat, double pressure);

// Saturates every open cell of the field.
void condense(Field& field, const globe::Planet& planet);

// The sea gives its air vapour as wind stirs the two, towards what the surface could hold; the
// land gives a share of that.
void evaporate(Field& field, const globe::Planet& planet, double seconds);

} // namespace hog::weather
