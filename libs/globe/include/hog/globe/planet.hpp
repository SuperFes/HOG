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

// How the planet's lid moves: plates that ride and sink, one lid that never moves, or one that
// founders now and then and is laid down again whole.
enum class Regime : std::uint8_t { Plates, StagnantLid, Episodic };

// Where the thick, high crust lies: scattered as continents, or gathered on one side.
enum class Layout : std::uint8_t { Scattered, OneSide };

// What can stand on the surface as liquid and ice.
enum class Volatile : std::uint8_t { None, Water, Methane };

struct Crust {
    Material rock = Material::Basalt;
    double thickness = 7'000.0; // metres
};

// A planet as physical knobs; Earth's by default, at the game's size. Presets carry a real
// body's physics, not its size, and gravity is set rather than grown from the radius.
struct Planet {
    std::uint64_t seed = 1;

    double radius = 115'000.0; // metres, to the reference level
    double gravity = 9.81;     // m/s²
    double day = 86'400.0;     // seconds, sunrise to sunrise
    double tilt = 23.44;       // degrees
    // Feature widths as a share of the same features on Earth, over the radius's share of
    // Earth's, so a small world keeps its landforms in proportion to its size.
    double lateral_scale = 1.0;

    // The voxel shell: impassable below the floor, open sky above the ceiling.
    double voxel = 2.5;           // metres
    double floor_depth = 4'000.0; // metres of rock kept under the lowest ground
    double sky_height = 10'000.0; // metres above the reference level

    Regime regime = Regime::Plates;
    Crust high{.rock = Material::Granite, .thickness = 35'000.0};
    Crust low{.rock = Material::Basalt, .thickness = 7'000.0};
    double high_fraction = 0.4; // of the surface
    Layout layout = Layout::Scattered;
    double mantle_density = 3'300.0; // kg/m³
    double heat_flow = 0.087;        // W/m² out through the surface

    double surface_age = 0.1; // Gyr since the surface last kept its craters
    double impact_flux = 1.0; // the Moon's rate as 1

    double pressure = 101'325.0;        // pascals at the reference level
    double equator_temperature = 300.0; // kelvin, the year's mean
    double pole_temperature = 245.0;
    Volatile volatiles = Volatile::Water;
    double sea_fraction = 0.7; // of the surface, where liquid can stand
    // How hard running water has worn the land before the world begins, as the drainage's erosion
    // rate; none at 0.
    double wear = 0.0;
};

// What follows from the knobs by known physics.
struct Derived {
    // How tall rock stands before its own weight crushes it, Y/(ρg), in the high crust.
    double strength_height = 0.0;
    double crater_transition = 0.0; // metres across, where simple bowls give way to complex
    double tallest = 0.0;           // metres, the highest a mountain can stand over its base
    double isostatic_step = 0.0;    // metres the high crust floats above the low, dry
    double geotherm = 0.0;          // K/m down through the crust
    double floor_temperature = 0.0; // kelvin
    double lateral = 0.0;           // feature widths as a share of Earth's
    double vertical = 0.0;          // heights as a share of those at the default radius
    bool liquid = false;            // the volatile is liquid somewhere on the surface
    bool ice = false;               // and frozen somewhere
    bool fluvial = false;
    bool glacial = false;
    bool aeolian = false;
};

[[nodiscard]] Derived derive(const Planet& planet);

// A number telling worlds apart: the same for the same knobs, so saves match their world.
[[nodiscard]] std::uint64_t world_id(const Planet& planet) noexcept;

// A planet as bytes, to send to a client that draws it, and back; the same planet, to the bit.
[[nodiscard]] std::vector<std::byte> encode_planet(const Planet& planet);
[[nodiscard]] Result<Planet> decode_planet(std::span<const std::byte> bytes);

// The year's mean surface temperature at a latitude, in radians.
[[nodiscard]] double temperature_at(const Planet& planet, double latitude);
[[nodiscard]] double mean_temperature(const Planet& planet);

// The pressure its vapour holds in balance with the liquid at a temperature; none for no volatile
// or above its critical point.
[[nodiscard]] std::optional<double> saturation_pressure(Volatile volatiles, double temperature);
// J/kg to turn the liquid to vapour, and kg/mol of its vapour.
[[nodiscard]] std::optional<double> latent_heat(Volatile volatiles);
[[nodiscard]] std::optional<double> molar_mass(Volatile volatiles);

// Where the volatile boils at a pressure, in kelvin; none below its triple point's pressure.
[[nodiscard]] std::optional<double> boiling_point(Volatile volatiles, double pressure);

// "earth", "mars", "venus" or "moon".
[[nodiscard]] std::optional<Planet> preset(std::string_view name);

// Whether every knob is one a planet can have.
[[nodiscard]] Result<void> validate(const Planet& planet);

// A `[globe]` table: an optional `preset`, then any knobs by name to change from it, the crusts as
// `[globe.high]` and `[globe.low]`.
[[nodiscard]] Result<Planet> parse_planet(std::string_view text);
[[nodiscard]] Result<Planet> load_planet(const std::filesystem::path& path);

} // namespace hog::globe
