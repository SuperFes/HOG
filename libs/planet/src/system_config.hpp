#pragma once

#include <hog/core/result.hpp>
#include <hog/planet/system.hpp>

#include <toml++/toml.hpp>

#include <string_view>

namespace hog::planet::detail {

// Reads the system's keys and tables from [planet] over `system`, leaving what's missing as it
// was:
//
//     [planet]
//     gravity = 9.81             # metres a second squared
//     water_depth = 2_620        # metres, the water as an even layer
//
//     [planet.air]
//     pressure_kpa = 101.325
//     oxygen = 0.2095            # share by volume
//     carbon_dioxide_ppm = 280
//
//     [planet.orbit]             # degrees, and Earth's years
//     distance_au = 1.0
//     eccentricity = 0.0167
//     inclination = 0.0
//     node = 0.0
//     periapsis = 102.94
//     longitude = 100.46
//     period_years = 1.0
//
//     [[planet.stars]]           # replaces the Sun; each orbit as the planet's
//     luminosity = 1.0
//     temperature = 5_772
//     radius_km = 695_700
//     tide = 0.28
//     orbit = { distance_au = 0.0 }
//
//     [[planet.moons]]           # replaces the Moon; moons = [] for none
//     radius_km = 1_737.4
//     albedo = 0.12
//     tide = 0.6
//     orbit = { distance_km = 384_400, period_days = 27.32 }
//
//     [planet.rings]             # none unless given
//     inner = 1.5                # planet radii
//     outer = 2.3
//     opacity = 0.5
//     albedo = 0.5
//
//     [[planet.planets]]         # replaces Earth's neighbours; planets = [] for none
//     radius_km = 6_051.8
//     albedo = 0.69
//     orbit = { distance_au = 0.723, period_years = 0.615 }
//
// Unknown keys are errors, as in the rest of [planet].
[[nodiscard]] Result<void> parse_system(const toml::table& planet, System& system);

// The keys parse_system reads straight from [planet].
[[nodiscard]] bool system_key(std::string_view key) noexcept;

} // namespace hog::planet::detail
