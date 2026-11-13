#include <hog/weather/field.hpp>
#include <hog/weather/snowpack.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::weather {

namespace {

constexpr double freezing = 273.15;
// Kelvin over and under freezing between all rain and all snow.
constexpr double sleet_span = 4.0;
constexpr double hours_a_year = 8'766.0;
constexpr double days_a_year = 365.25;
// Metres of water melted a day per kelvin over freezing, for clean ice.
constexpr double melt_factor = 0.005;
constexpr double water_density = 1'000.0;
constexpr double ice_density = 917.0;

} // namespace

double snow_share(double temperature) {
    return std::clamp(0.5 + ((freezing - temperature) / sleet_span), 0.0, 1.0);
}

double warm_kelvin(double mean, double swing) {
    if (swing <= 0.0) {
        return std::max(0.0, mean);
    }
    if (mean >= swing) {
        return mean;
    }
    if (mean <= -swing) {
        return 0.0;
    }
    // The part of the sine over zero runs from asin(-mean/swing) to its mirror.
    const double rise = std::asin(-mean / swing);
    const double area = (mean * (std::numbers::pi - (2.0 * rise))) +
                        (2.0 * std::sqrt((swing * swing) - (mean * mean)));
    return area / (2.0 * std::numbers::pi);
}

Balance ice_balance(const globe::Planet& planet, const Climate& climate, double height) {
    const double mean = air_temperature(planet, climate.surface, height);
    // Snow falls on the cold side of the season; treat the year's mean as the temperature it falls
    // at.
    const double water = climate.precipitation * hours_a_year / 1'000.0 * snow_share(mean);
    return {.gain = water * water_density / ice_density,
            .loss = melt_factor * days_a_year * warm_kelvin(mean - freezing, climate.swing) *
                    water_density / ice_density};
}

std::optional<double> snowline(const globe::Planet& planet, const Climate& climate, double top) {
    if (ice_balance(planet, climate, top).net() <= 0.0) {
        return std::nullopt;
    }
    // Balance only improves with height, so the line is found by halving.
    double low = 0.0;
    double high = top;
    if (ice_balance(planet, climate, low).net() > 0.0) {
        return low;
    }
    for (int step = 0; step < 40; ++step) {
        const double mid = 0.5 * (low + high);
        (ice_balance(planet, climate, mid).net() > 0.0 ? high : low) = mid;
    }
    return high;
}

} // namespace hog::weather
