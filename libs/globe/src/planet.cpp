#include <hog/globe/planet.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::globe {
namespace {

constexpr double earth_radius = 6'371'000.0;
constexpr double earth_gravity = 9.81;
// Rock conducts heat about this well through the crust, W/m/K.
constexpr double conductivity = 2.5;
constexpr double gas_constant = 8.314'462;

double strength_height(const Crust& crust, double gravity) {
    const Properties& rock = properties(crust.rock);
    return rock.strength / (rock.density * gravity);
}

// Both scale with Y/(ρg); fitted to Earth's granite crust: simple craters to 3.2 km across, and
// its highest mountain at 8.8 km.
constexpr double earth_transition = 3'200.0;
constexpr double earth_tallest = 8'800.0;

struct Phase {
    double triple_temperature; // kelvin
    double triple_pressure;    // pascals
    double critical_temperature;
    double latent_heat; // J/mol to boil, its mean over the liquid's range
};

std::optional<Phase> phase_of(Volatile volatiles) {
    switch (volatiles) {
    case Volatile::None:
        return std::nullopt;
    case Volatile::Water:
        return Phase{.triple_temperature = 273.16,
                     .triple_pressure = 611.657,
                     .critical_temperature = 647.096,
                     .latent_heat = 42'850.0};
    case Volatile::Methane:
        return Phase{.triple_temperature = 90.694,
                     .triple_pressure = 11'696.0,
                     .critical_temperature = 190.564,
                     .latent_heat = 8'500.0};
    }
    return std::nullopt;
}

Planet mars() {
    return {.gravity = 3.71,
            .day = 88'775.0,
            .tilt = 25.19,
            .sky_height = 25'000.0,
            .regime = Regime::StagnantLid,
            .high = {.rock = Material::Basalt, .thickness = 58'000.0},
            .low = {.rock = Material::Basalt, .thickness = 32'000.0},
            .high_fraction = 0.6,
            .layout = Layout::OneSide,
            .mantle_density = 3'500.0,
            .heat_flow = 0.02,
            .surface_age = 3.5,
            .impact_flux = 2.0,
            .pressure = 636.0,
            .equator_temperature = 224.0,
            .pole_temperature = 150.0,
            .volatiles = Volatile::Water,
            .sea_fraction = 0.0};
}

Planet venus() {
    return {.gravity = 8.87,
            .day = 10'087'200.0,
            .tilt = 2.64,
            .sky_height = 12'000.0,
            .regime = Regime::Episodic,
            .high = {.rock = Material::Basalt, .thickness = 30'000.0},
            .low = {.rock = Material::Basalt, .thickness = 30'000.0},
            .high_fraction = 0.0,
            .heat_flow = 0.04,
            .surface_age = 0.5,
            .pressure = 9'200'000.0,
            .equator_temperature = 737.0,
            .pole_temperature = 737.0,
            .volatiles = Volatile::None,
            .sea_fraction = 0.0};
}

Planet moon() {
    return {.gravity = 1.62,
            .day = 2'551'443.0,
            .tilt = 1.54,
            .sky_height = 12'000.0,
            .regime = Regime::StagnantLid,
            .high = {.rock = Material::Anorthosite, .thickness = 50'000.0},
            .low = {.rock = Material::Basalt, .thickness = 30'000.0},
            .high_fraction = 0.84,
            .layout = Layout::OneSide,
            .mantle_density = 3'360.0,
            .heat_flow = 0.02,
            .surface_age = 4.0,
            .pressure = 0.0,
            .equator_temperature = 295.0,
            .pole_temperature = 100.0,
            .volatiles = Volatile::None,
            .sea_fraction = 0.0};
}

} // namespace

double temperature_at(const Planet& planet, double latitude) {
    return planet.pole_temperature +
           ((planet.equator_temperature - planet.pole_temperature) * std::cos(latitude));
}

double mean_temperature(const Planet& planet) {
    // cos(latitude) averages π/4 over a sphere's area.
    return planet.pole_temperature +
           ((planet.equator_temperature - planet.pole_temperature) * std::numbers::pi / 4.0);
}

std::optional<double> boiling_point(Volatile volatiles, double pressure) {
    const auto phase = phase_of(volatiles);
    if (!phase || pressure < phase->triple_pressure) {
        return std::nullopt;
    }
    // Clausius–Clapeyron from the triple point.
    const double inverse =
        (1.0 / phase->triple_temperature) -
        (gas_constant / phase->latent_heat * std::log(pressure / phase->triple_pressure));
    return std::min(1.0 / inverse, phase->critical_temperature);
}

Derived derive(const Planet& planet) {
    Derived derived;
    derived.strength_height = strength_height(planet.high, planet.gravity);
    const double earth = strength_height({.rock = Material::Granite}, earth_gravity);
    derived.crater_transition = earth_transition * derived.strength_height / earth;
    derived.tallest = earth_tallest * derived.strength_height / earth;
    const auto floats = [&](const Crust& crust) {
        return crust.thickness * (1.0 - (properties(crust.rock).density / planet.mantle_density));
    };
    derived.isostatic_step = floats(planet.high) - floats(planet.low);
    derived.geotherm = planet.heat_flow / conductivity;
    derived.floor_temperature = mean_temperature(planet) + (derived.geotherm * planet.floor_depth);
    derived.lateral = planet.lateral_scale * planet.radius / earth_radius;

    const double warmest = std::max(planet.equator_temperature, planet.pole_temperature);
    const double coldest = std::min(planet.equator_temperature, planet.pole_temperature);
    if (const auto phase = phase_of(planet.volatiles)) {
        const auto boils = boiling_point(planet.volatiles, planet.pressure);
        derived.liquid = boils && warmest >= phase->triple_temperature && coldest < *boils;
        derived.ice = coldest < phase->triple_temperature;
    }
    derived.fluvial = derived.liquid;
    derived.glacial = derived.ice;
    // Pluto's dunes formed under about a pascal of air.
    derived.aeolian = planet.pressure >= 1.0;
    derived.layers =
        layers_about(planet.radius, planet.floor_depth, planet.sky_height, planet.voxel);
    return derived;
}

std::optional<Planet> preset(std::string_view name) {
    if (name == "earth") {
        return Planet{};
    }
    if (name == "mars") {
        return mars();
    }
    if (name == "venus") {
        return venus();
    }
    if (name == "moon") {
        return moon();
    }
    return std::nullopt;
}

} // namespace hog::globe
