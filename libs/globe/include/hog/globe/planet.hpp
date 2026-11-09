#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/formation/body.hpp>
#include <hog/formation/climate.hpp>
#include <hog/globe/material.hpp>
#include <hog/planet/system.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace hog::globe {

// The working world's formation: the formation's own knobs, turning in four hours.
[[nodiscard]] inline formation::Body working_body() {
    formation::Body body;
    body.day = 4.0;
    return body;
}

// A planet as its knobs, each in one place: what it's made of and how it formed, its place among
// its stars, and the voxel shell it's held in. The defaults are the working world; its liquid is
// water and its air is air.
struct Planet {
    // Its size, spin, gravity, rock, climate and everything that shaped its land.
    formation::Body body = working_body();
    // Its stars, orbit, moons, rings, neighbours and air.
    planet::System system;

    // The voxel shell: impassable below the floor, open sky above the ceiling.
    double voxel = 0.333'333'333; // metres
    double floor_depth = 2'500.0; // metres of rock kept under the lowest ground
    double sky_height = 12'000.0; // metres above the reference level

    double heat_flow = 0.087; // W/m² out through the surface
    // Splits of the sphere the land is formed on, stage by stage from plates to soil
    // (hog::formation), its heights, ice, lakes and layers then read from it.
    double formed_detail = 5.0;
};

// The formation's knobs as the planet sets them, with what its stars set filled in.
[[nodiscard]] formation::Body formation_body(const Planet& planet);

// One turn of day and night in the game's time, which runs with real time.
[[nodiscard]] GameDuration day_length(const Planet& planet);

// W/m² of starlight where the planet orbits.
[[nodiscard]] double star_flux(const planet::System& system);

// What follows from the knobs by known physics.
struct Derived {
    double geotherm = 0.0;          // K/m down through the crust
    double floor_temperature = 0.0; // kelvin
    bool liquid = false;            // water is liquid somewhere on the surface
    bool ice = false;               // and frozen somewhere
};

[[nodiscard]] Derived derive(const Planet& planet);

// A number telling worlds apart: the same for the same knobs, so saves match their world.
[[nodiscard]] std::uint64_t world_id(const Planet& planet) noexcept;

// A planet as bytes, to send to a client that draws it, and back; the same planet, to the bit.
[[nodiscard]] std::vector<std::byte> encode_planet(const Planet& planet);
[[nodiscard]] Result<Planet> decode_planet(std::span<const std::byte> bytes);

// The year's mean surface temperature at a latitude, in radians, at the sea's level: the
// formation's climate.
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

// The pressure water vapour holds in balance with water at a temperature; none above its
// critical point.
[[nodiscard]] std::optional<double> saturation_pressure(double temperature);
// J/kg to turn water to vapour, and kg/mol of the vapour.
inline constexpr double latent_heat = 42'850.0 / 0.018'015;
inline constexpr double molar_mass = 0.018'015;

// Where water boils at a pressure, in kelvin; none below its triple point's pressure.
[[nodiscard]] std::optional<double> boiling_point(double pressure);

// Whether every knob is one a planet can have.
[[nodiscard]] Result<void> validate(const Planet& planet);

// A `[planet]` table: any knobs by name to change from the working world's.
[[nodiscard]] Result<Planet> parse_planet(std::string_view text);
[[nodiscard]] Result<Planet> load_planet(const std::filesystem::path& path);

} // namespace hog::globe
