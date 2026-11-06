#pragma once

#include <hog/globe/planet.hpp>

#include <optional>

namespace hog::weather {

// Year-round climate of one spot, as the weather gives it: the surface's mean temperature reduced
// to the reference level, how far the seasons swing it either way, and the mean precipitation.
struct Climate {
    double surface = 273.15;  // kelvin
    double swing = 10.0;      // kelvin, amplitude of the seasons around the mean
    double precipitation = 0; // mm/h, mean
};

// What a spot gains and loses in a year, as metres of ice.
struct Balance {
    double gain = 0.0;
    double loss = 0.0;
    [[nodiscard]] double net() const noexcept { return gain - loss; }
};

// Share of precipitation that falls as snow in air at `temperature` kelvin.
[[nodiscard]] double snow_share(double temperature);

// Mean over a year of the kelvin a sinusoidal season sits over freezing, counting only the warm
// part: the melt season's strength.
[[nodiscard]] double warm_kelvin(double mean, double swing);

// Snow settling and ice melting on ground `height` metres over the reference level.
[[nodiscard]] Balance ice_balance(const globe::Planet& planet, const Climate& climate,
                                  double height);

// Lowest height under `top` at which a year leaves snow behind; none where it all melts.
[[nodiscard]] std::optional<double> snowline(const globe::Planet& planet, const Climate& climate,
                                             double top);

} // namespace hog::weather
