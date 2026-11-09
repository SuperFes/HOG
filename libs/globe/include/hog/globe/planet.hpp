#pragma once

#include <hog/core/result.hpp>
#include <hog/globe/material.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace hog::globe {

// What can stand on the surface as liquid and ice.
enum class Volatile : std::uint8_t { None, Water, Methane };

// A planet as physical knobs, at the game's size; gravity is set rather than grown from the
// radius.
struct Planet {
    std::uint64_t seed = 1;

    double radius = 115'000.0; // metres, to the reference level
    double gravity = 9.81;     // m/s²
    double day = 86'400.0;     // seconds, sunrise to sunrise
    double tilt = 23.44;       // degrees

    // The voxel shell: impassable below the floor, open sky above the ceiling.
    double voxel = 2.5;           // metres
    double floor_depth = 4'000.0; // metres of rock kept under the lowest ground
    double sky_height = 10'000.0; // metres above the reference level

    double heat_flow = 0.087; // W/m² out through the surface

    double pressure = 101'325.0;        // pascals at the reference level
    double equator_temperature = 300.0; // kelvin, the year's mean
    double pole_temperature = 245.0;
    Volatile volatiles = Volatile::Water;
    // Splits of the sphere the land is formed on, stage by stage from plates to soil
    // (hog::formation), its heights, ice, lakes and layers then read from it.
    double formed_detail = 5.0;
};

// What follows from the knobs by known physics.
struct Derived {
    double geotherm = 0.0;          // K/m down through the crust
    double floor_temperature = 0.0; // kelvin
    double vertical = 0.0;          // heights as a share of those at the default radius
    bool liquid = false;            // the volatile is liquid somewhere on the surface
    bool ice = false;               // and frozen somewhere
};

[[nodiscard]] Derived derive(const Planet& planet);

// A number telling worlds apart: the same for the same knobs, so saves match their world.
[[nodiscard]] std::uint64_t world_id(const Planet& planet) noexcept;

// A planet as bytes, to send to a client that draws it, and back; the same planet, to the bit.
[[nodiscard]] std::vector<std::byte> encode_planet(const Planet& planet);
[[nodiscard]] Result<Planet> decode_planet(std::span<const std::byte> bytes);

// The year's mean surface temperature at a latitude, in radians.
[[nodiscard]] double temperature_at(const Planet& planet, double latitude);

// How fast still air cools with height, K/m; none where there's no air.
[[nodiscard]] double climate_lapse_rate(const Planet& planet);

// What a point is in, for its temperature.
enum class Surroundings : std::uint8_t {
    Inside, // in the ground
    Water,  // under standing water
    Open,   // in the open air
};

// Kelvin at `radius` from the planet's centre, where the ground under it is `ground` from the
// centre and the climate gives `at_sea_level` kelvin at its latitude (temperature_at), with
// nothing but that, the height and the depth to go by: no weather, sun or local heat. Air cools
// with height from the surface's temperature; ground warms with depth by the geotherm; water
// takes the surface's.
[[nodiscard]] double baseline_temperature(const Planet& planet, const Derived& derived,
                                          double at_sea_level, double radius, double ground,
                                          Surroundings surroundings);
[[nodiscard]] double mean_temperature(const Planet& planet);

// The pressure its vapour holds in balance with the liquid at a temperature; none for no volatile
// or above its critical point.
[[nodiscard]] std::optional<double> saturation_pressure(Volatile volatiles, double temperature);
// J/kg to turn the liquid to vapour, and kg/mol of its vapour.
[[nodiscard]] std::optional<double> latent_heat(Volatile volatiles);
[[nodiscard]] std::optional<double> molar_mass(Volatile volatiles);

// Where the volatile boils at a pressure, in kelvin; none below its triple point's pressure.
[[nodiscard]] std::optional<double> boiling_point(Volatile volatiles, double pressure);

// Whether every knob is one a planet can have.
[[nodiscard]] Result<void> validate(const Planet& planet);

// A `[globe]` table: any knobs by name to change from the defaults.
[[nodiscard]] Result<Planet> parse_planet(std::string_view text);
[[nodiscard]] Result<Planet> load_planet(const std::filesystem::path& path);

} // namespace hog::globe
