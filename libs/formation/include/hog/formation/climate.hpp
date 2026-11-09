#pragma once

#include <hog/formation/body.hpp>
#include <hog/formation/sphere.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::formation {

// The climate a planet's shape gives it, worked out once rather than stepped through: yearly
// means, enough to tell where water falls and where ice and plants can be.
struct Climate {
    std::vector<double> temperature; // °C at the ground, through the year
    std::vector<double> rain;        // metres of water a year
    std::vector<DVec3> wind;         // the prevailing wind's way along the surface, unit length
};

// Yearly mean temperature by latitude, from sunlight against what the planet radiates and
// carries towards its poles (a Budyko–Sellers balance), at the liquid's level.
[[nodiscard]] double sea_level_temperature(const Body& body, double latitude);

// The same worked out once for every latitude, °C a degree apart from the south pole to the north,
// to read many times.
using LatitudeTemperatures = std::array<double, 181>;
[[nodiscard]] LatitudeTemperatures sea_level_temperatures(const Body& body);
[[nodiscard]] double temperature_at(const LatitudeTemperatures& rows, double latitude) noexcept;

// `heights` are metres from the liquid's level; `sea` is 1 where open liquid lies, which moisture
// comes off.
[[nodiscard]] Climate climate_on(const Sphere& sphere, std::span<const double> heights,
                                 std::span<const std::uint8_t> sea, const Body& body);
// As above, with open liquid wherever the height is under 0.
[[nodiscard]] Climate climate_on(const Sphere& sphere, std::span<const double> heights,
                                 const Body& body);

} // namespace hog::formation
