#include "planet_knobs.hpp"

#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/maths.hpp>
#include <hog/globe/planet.hpp>
#include <hog/planet/ephemeris.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <utility>

namespace hog::globe {
namespace {

// Rock conducts heat about this well through the crust, W/m/K.
constexpr double conductivity = 2.5;
constexpr double gas_constant = 8.314'462;
constexpr double kelvin = 273.15;

// Water's triple and critical points.
constexpr double triple_temperature = 273.16;
constexpr double triple_pressure = 611.657;
constexpr double critical_temperature = 647.096;
constexpr double latent_per_mole = latent_heat * molar_mass;

std::unexpected<Error> invalid(std::string_view message) {
    return make_error(ErrorCode::InvalidArgument, std::string{message});
}

// NaN fails every comparison, so these reject it too.
bool positive(double value) {
    return value > 0.0 && std::isfinite(value);
}

bool within(double value, double low, double high) {
    return value >= low && value <= high;
}

// The climate's temperatures by latitude, worked out once for the last planet asked about on
// each thread, as the weather reads them column by column.
const formation::LatitudeTemperatures& temperatures(const Planet& planet) {
    struct Kept {
        formation::Body body;
        formation::LatitudeTemperatures rows{};
        bool any = false;
    };
    thread_local Kept kept;
    const formation::Body body = formation_body(planet);
    const auto same = [&] {
        return kept.any && kept.body.star_flux == body.star_flux && kept.body.tilt == body.tilt &&
               kept.body.albedo == body.albedo && kept.body.ice_albedo == body.ice_albedo &&
               kept.body.greenhouse == body.greenhouse &&
               kept.body.heat_transport == body.heat_transport;
    };
    if (!same()) {
        kept.body = body;
        kept.rows = formation::sea_level_temperatures(body);
        kept.any = true;
    }
    return kept.rows;
}

} // namespace

double star_flux(const planet::System& system) {
    // Each star as if at the system's centre, the planet at its orbit's mean distance.
    const double away = std::max(system.orbit.distance, 1.0) / planet::astronomical_unit;
    double sum = 0.0;
    for (const planet::Star& star : system.stars) {
        sum += star.luminosity;
    }
    return planet::solar_constant * sum / (away * away);
}

GameDuration day_length(const Planet& planet) {
    return std::chrono::round<GameDuration>(
        std::chrono::duration<double, std::ratio<3'600>>{planet.body.day});
}

formation::Body formation_body(const Planet& planet) {
    formation::Body body = planet.body;
    body.star_flux = star_flux(planet.system);
    return body;
}

double temperature_at(const Planet& planet, double latitude) {
    return formation::temperature_at(temperatures(planet), latitude) + kelvin;
}

double climate_lapse_rate(const Planet& planet) {
    return planet.system.air.pressure > 0.0 ? planet.body.lapse_rate : 0.0;
}

double baseline_temperature(const Planet& planet, const Derived& derived, double at_sea_level,
                            double radius, double ground, Surroundings surroundings) {
    const double lapse = climate_lapse_rate(planet);
    const double level = planet.body.radius;
    const double surface = at_sea_level - (lapse * std::max(0.0, ground - level));
    switch (surroundings) {
    case Surroundings::Inside:
        return surface + (derived.geotherm * std::max(0.0, ground - radius));
    case Surroundings::Water:
        return surface;
    case Surroundings::Open:
        break;
    }
    return at_sea_level - (lapse * std::max(0.0, radius - level));
}

double mean_temperature(const Planet& planet) {
    const formation::LatitudeTemperatures& rows = temperatures(planet);
    double sum = 0.0;
    double weights = 0.0;
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const double weight =
            maths::cos((static_cast<double>(row) - 90.0) * std::numbers::pi / 180.0);
        sum += weight * rows[row];
        weights += weight;
    }
    return (sum / weights) + kelvin;
}

std::optional<double> saturation_pressure(double temperature) {
    if (temperature <= 0.0 || temperature > critical_temperature) {
        return std::nullopt;
    }
    return triple_pressure * maths::exp(-latent_per_mole / gas_constant *
                                        ((1.0 / temperature) - (1.0 / triple_temperature)));
}

std::optional<double> boiling_point(double pressure) {
    if (pressure < triple_pressure) {
        return std::nullopt;
    }
    // Clausius–Clapeyron from the triple point.
    const double inverse = (1.0 / triple_temperature) - (gas_constant / latent_per_mole *
                                                         maths::log(pressure / triple_pressure));
    return std::min(1.0 / inverse, critical_temperature);
}

Derived derive(const Planet& planet) {
    Derived derived;
    derived.geotherm = planet.heat_flow / conductivity;
    derived.floor_temperature = mean_temperature(planet) + (derived.geotherm * planet.floor_depth);

    const formation::LatitudeTemperatures& rows = temperatures(planet);
    const double warmest = std::ranges::max(rows) + kelvin;
    const double coldest = std::ranges::min(rows) + kelvin;
    const auto boils = boiling_point(planet.system.air.pressure);
    derived.liquid = boils && warmest >= triple_temperature && coldest < *boils;
    derived.ice = coldest < triple_temperature;
    return derived;
}

Result<void> validate(const Planet& planet) {
    const formation::Body& body = planet.body;
    const double shell = planet.floor_depth + planet.sky_height;
    const auto checks = std::to_array<std::pair<bool, std::string_view>>({
        {positive(body.radius), "radius must be positive"},
        {positive(body.gravity), "gravity must be positive"},
        {positive(body.day), "day must be positive hours"},
        {within(body.tilt, 0.0, 90.0), "tilt must be 0 to 90 degrees"},
        {positive(planet.voxel), "voxel must be positive"},
        {positive(planet.floor_depth) && planet.floor_depth < body.radius,
         "floor_depth must be positive and less than the radius"},
        {positive(planet.sky_height), "sky_height must be positive"},
        {planet.voxel <= std::min(planet.floor_depth, planet.sky_height) &&
             shell / planet.voxel < std::numeric_limits<int>::max(),
         "voxel must fit the shell, and the shell not hold too many layers"},
        {within(planet.heat_flow, 0.0, 1e3), "heat_flow must be 0 to 1000 W/m²"},
        {within(planet.formed_detail, 4.0, 9.0) &&
             planet.formed_detail == std::floor(planet.formed_detail),
         "formed_detail must be a whole 4 to 9 splits"},
    });
    for (const auto& [holds, message] : checks) {
        if (!holds) {
            return invalid(std::format("planet.{}", message));
        }
    }
    for (const auto& [name, member] : knobs::body_numbers) {
        if (!std::isfinite(body.*member)) {
            return invalid(std::format("planet.{} must be a number", name));
        }
    }
    for (const auto& [name, member] : knobs::ores) {
        if (!std::isfinite(body.ore_abundance.*member) || body.ore_abundance.*member < 0.0) {
            return invalid(std::format("planet.ore_abundance.{} must be 0 or more", name));
        }
    }
    return {};
}

namespace {

// The knobs' version in the bytes, so a planet from another is refused, not misread.
constexpr std::uint8_t planet_bytes_version = 5;

void write_double(ByteWriter& out, double value) {
    out.write(std::bit_cast<std::uint64_t>(value));
}

// Every knob in a fixed order, the system's last.
void write_knobs(ByteWriter& out, const Planet& planet) {
    out.write(planet.body.seed);
    for (const auto& knob : knobs::body_numbers) {
        write_double(out, planet.body.*knob.second);
    }
    for (const auto& knob : knobs::body_counts) {
        out.write_signed(planet.body.*knob.second);
    }
    for (const auto& knob : knobs::ores) {
        write_double(out, planet.body.ore_abundance.*knob.second);
    }
    for (const auto& knob : knobs::shell) {
        write_double(out, planet.*knob.second);
    }
    planet::write_system(out, planet.system);
}

} // namespace

std::uint64_t world_id(const Planet& planet) noexcept {
    ByteWriter out;
    write_knobs(out, planet);
    const std::vector<std::byte> bytes = std::move(out).take();
    std::uint64_t id = hash_values(bytes.size());
    for (std::size_t at = 0; at < bytes.size(); at += sizeof(std::uint64_t)) {
        std::uint64_t word = 0;
        for (std::size_t byte = at; byte < std::min(at + sizeof(std::uint64_t), bytes.size());
             ++byte) {
            word = (word << 8U) | static_cast<std::uint64_t>(bytes[byte]);
        }
        id = hash_values(id, word);
    }
    return id;
}

std::vector<std::byte> encode_planet(const Planet& planet) {
    ByteWriter out;
    out.write(planet_bytes_version);
    write_knobs(out, planet);
    return std::move(out).take();
}

Result<Planet> decode_planet(std::span<const std::byte> bytes) {
    ByteReader in{bytes};
    const auto cut = [] { return make_error(ErrorCode::Parse, "a planet's bytes cut short"); };
    const auto version = in.read<std::uint8_t>();
    if (!version || *version != planet_bytes_version) {
        return make_error(ErrorCode::Unsupported, "a planet of another version");
    }
    Planet planet;
    const auto read_double = [&in](double& into) {
        const auto bits = in.read<std::uint64_t>();
        if (!bits) {
            return false;
        }
        into = std::bit_cast<double>(*bits);
        return true;
    };
    const auto seed = in.read<std::uint64_t>();
    if (!seed) {
        return cut();
    }
    planet.body.seed = *seed;
    for (const auto& knob : knobs::body_numbers) {
        if (!read_double(planet.body.*knob.second)) {
            return cut();
        }
    }
    for (const auto& knob : knobs::body_counts) {
        const auto count = in.read_signed();
        if (!count || *count < std::numeric_limits<int>::min() ||
            *count > std::numeric_limits<int>::max()) {
            return cut();
        }
        planet.body.*knob.second = static_cast<int>(*count);
    }
    for (const auto& knob : knobs::ores) {
        if (!read_double(planet.body.ore_abundance.*knob.second)) {
            return cut();
        }
    }
    for (const auto& knob : knobs::shell) {
        if (!read_double(planet.*knob.second)) {
            return cut();
        }
    }
    if (!planet::read_system(in, planet.system)) {
        return cut();
    }
    if (in.remaining() != 0) {
        return make_error(ErrorCode::Parse, "a planet's bytes with bytes left over");
    }
    return planet;
}

} // namespace hog::globe
