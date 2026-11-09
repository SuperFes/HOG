#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/globe/maths.hpp>
#include <hog/globe/planet.hpp>

#include <algorithm>
#include <array>
#include <bit>
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

struct Phase {
    double triple_temperature; // kelvin
    double triple_pressure;    // pascals
    double critical_temperature;
    double latent_heat; // J/mol to boil, its mean over the liquid's range
    double molar_mass;  // kg/mol
};

std::optional<Phase> phase_of(Volatile volatiles) {
    switch (volatiles) {
    case Volatile::None:
        return std::nullopt;
    case Volatile::Water:
        return Phase{.triple_temperature = 273.16,
                     .triple_pressure = 611.657,
                     .critical_temperature = 647.096,
                     .latent_heat = 42'850.0,
                     .molar_mass = 0.018'015};
    case Volatile::Methane:
        return Phase{.triple_temperature = 90.694,
                     .triple_pressure = 11'696.0,
                     .critical_temperature = 190.564,
                     .latent_heat = 8'500.0,
                     .molar_mass = 0.016'043};
    }
    return std::nullopt;
}

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

} // namespace

double temperature_at(const Planet& planet, double latitude) {
    return planet.pole_temperature +
           ((planet.equator_temperature - planet.pole_temperature) * maths::cos(latitude));
}

double climate_lapse_rate(const Planet& planet) {
    // Earth's moist air at 6.5 K/km, scaled by gravity, as air cools by g/cp as it rises.
    constexpr double earth_lapse = 0.0065;
    return planet.pressure > 0.0 ? earth_lapse * planet.gravity / 9.81 : 0.0;
}

double baseline_temperature(const Planet& planet, const Derived& derived, double at_sea_level,
                            double radius, double ground, Surroundings surroundings) {
    const double lapse = climate_lapse_rate(planet);
    const double surface = at_sea_level - (lapse * std::max(0.0, ground - planet.radius));
    switch (surroundings) {
    case Surroundings::Inside:
        return surface + (derived.geotherm * std::max(0.0, ground - radius));
    case Surroundings::Water:
        return surface;
    case Surroundings::Open:
        break;
    }
    return at_sea_level - (lapse * std::max(0.0, radius - planet.radius));
}

double mean_temperature(const Planet& planet) {
    // cos(latitude) averages π/4 over a sphere's area.
    return planet.pole_temperature +
           ((planet.equator_temperature - planet.pole_temperature) * std::numbers::pi / 4.0);
}

std::optional<double> saturation_pressure(Volatile volatiles, double temperature) {
    const auto phase = phase_of(volatiles);
    if (!phase || temperature <= 0.0 || temperature > phase->critical_temperature) {
        return std::nullopt;
    }
    return phase->triple_pressure *
           maths::exp(-phase->latent_heat / gas_constant *
                      ((1.0 / temperature) - (1.0 / phase->triple_temperature)));
}

std::optional<double> latent_heat(Volatile volatiles) {
    const auto phase = phase_of(volatiles);
    return phase ? std::optional{phase->latent_heat / phase->molar_mass} : std::nullopt;
}

std::optional<double> molar_mass(Volatile volatiles) {
    const auto phase = phase_of(volatiles);
    return phase ? std::optional{phase->molar_mass} : std::nullopt;
}

std::optional<double> boiling_point(Volatile volatiles, double pressure) {
    const auto phase = phase_of(volatiles);
    if (!phase || pressure < phase->triple_pressure) {
        return std::nullopt;
    }
    // Clausius–Clapeyron from the triple point.
    const double inverse =
        (1.0 / phase->triple_temperature) -
        (gas_constant / phase->latent_heat * maths::log(pressure / phase->triple_pressure));
    return std::min(1.0 / inverse, phase->critical_temperature);
}

Derived derive(const Planet& planet) {
    Derived derived;
    // Heights are tuned at the game's size and keep their proportion to the planet's width at
    // any other.
    derived.vertical = planet.radius / Planet{}.radius;
    derived.geotherm = planet.heat_flow / conductivity;
    derived.floor_temperature = mean_temperature(planet) + (derived.geotherm * planet.floor_depth);

    const double warmest = std::max(planet.equator_temperature, planet.pole_temperature);
    const double coldest = std::min(planet.equator_temperature, planet.pole_temperature);
    if (const auto phase = phase_of(planet.volatiles)) {
        const auto boils = boiling_point(planet.volatiles, planet.pressure);
        derived.liquid = boils && warmest >= phase->triple_temperature && coldest < *boils;
        derived.ice = coldest < phase->triple_temperature;
    }
    return derived;
}

Result<void> validate(const Planet& planet) {
    const double shell = planet.floor_depth + planet.sky_height;
    const auto checks = std::to_array<std::pair<bool, std::string_view>>({
        {positive(planet.radius), "radius must be positive"},
        {positive(planet.gravity), "gravity must be positive"},
        {positive(planet.day), "day must be positive"},
        {within(planet.tilt, 0.0, 180.0), "tilt must be 0 to 180 degrees"},
        {positive(planet.voxel), "voxel must be positive"},
        {positive(planet.floor_depth) && planet.floor_depth < planet.radius,
         "floor_depth must be positive and less than the radius"},
        {positive(planet.sky_height), "sky_height must be positive"},
        {planet.voxel <= std::min(planet.floor_depth, planet.sky_height) &&
             shell / planet.voxel < std::numeric_limits<int>::max(),
         "voxel must fit the shell, and the shell not hold too many layers"},
        {within(planet.heat_flow, 0.0, 1e3), "heat_flow must be 0 to 1000 W/m²"},
        {within(planet.pressure, 0.0, 1e9), "pressure must be 0 to 1e9 Pa"},
        {positive(planet.equator_temperature) && positive(planet.pole_temperature),
         "temperatures must be positive kelvin"},
        {within(planet.formed_detail, 4.0, 9.0) &&
             planet.formed_detail == std::floor(planet.formed_detail),
         "formed_detail must be a whole 4 to 9 splits"},
    });
    for (const auto& [holds, message] : checks) {
        if (!holds) {
            return invalid(std::format("globe.{}", message));
        }
    }
    return {};
}

std::uint64_t world_id(const Planet& planet) noexcept {
    const auto bits = [](double value) { return std::bit_cast<std::uint64_t>(value); };
    return hash_values(planet.seed, bits(planet.radius), bits(planet.gravity), bits(planet.day),
                       bits(planet.tilt), bits(planet.voxel), bits(planet.floor_depth),
                       bits(planet.sky_height), bits(planet.heat_flow), bits(planet.pressure),
                       bits(planet.equator_temperature), bits(planet.pole_temperature),
                       static_cast<int>(planet.volatiles), bits(planet.formed_detail));
}

namespace {

// The knobs' version in the bytes, so a planet from another is refused, not misread.
constexpr std::uint8_t planet_bytes_version = 4;

void write_double(ByteWriter& out, double value) {
    out.write(std::bit_cast<std::uint64_t>(value));
}

} // namespace

std::vector<std::byte> encode_planet(const Planet& planet) {
    ByteWriter out;
    out.write(planet_bytes_version);
    out.write(planet.seed);
    for (const double value :
         {planet.radius, planet.gravity, planet.day, planet.tilt, planet.voxel, planet.floor_depth,
          planet.sky_height, planet.heat_flow, planet.pressure, planet.equator_temperature,
          planet.pole_temperature}) {
        write_double(out, value);
    }
    out.write(static_cast<std::uint8_t>(planet.volatiles));
    write_double(out, planet.formed_detail);
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
    const auto seed = in.read<std::uint64_t>();
    if (!seed) {
        return cut();
    }
    planet.seed = *seed;
    const auto read_double = [&in](double& into) {
        const auto bits = in.read<std::uint64_t>();
        if (!bits) {
            return false;
        }
        into = std::bit_cast<double>(*bits);
        return true;
    };
    const auto read_byte = [&in](std::uint8_t& into) {
        const auto value = in.read<std::uint8_t>();
        if (!value) {
            return false;
        }
        into = *value;
        return true;
    };
    std::uint8_t volatiles = 0;
    const bool whole = read_double(planet.radius) && read_double(planet.gravity) &&
                       read_double(planet.day) && read_double(planet.tilt) &&
                       read_double(planet.voxel) && read_double(planet.floor_depth) &&
                       read_double(planet.sky_height) && read_double(planet.heat_flow) &&
                       read_double(planet.pressure) && read_double(planet.equator_temperature) &&
                       read_double(planet.pole_temperature) && read_byte(volatiles) &&
                       read_double(planet.formed_detail);
    if (!whole) {
        return cut();
    }
    if (volatiles > static_cast<std::uint8_t>(Volatile::Methane)) {
        return make_error(ErrorCode::Parse, "a planet with a kind it doesn't know");
    }
    planet.volatiles = static_cast<Volatile>(volatiles);
    if (in.remaining() != 0) {
        return make_error(ErrorCode::Parse, "a planet's bytes with bytes left over");
    }
    return planet;
}

} // namespace hog::globe
