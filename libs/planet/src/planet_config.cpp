#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/core/text_file.hpp>
#include <hog/planet/planet_config.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <string>

namespace hog::planet {

namespace {

using namespace std::string_view_literals;

constexpr std::array known_keys{
    "seed"sv,      "ocean_fraction"sv, "population"sv, "settled_density"sv, "wilderness_factor"sv,
    "radius_km"sv, "day_hours"sv,      "axial_tilt"sv};

constexpr double min_day_hours = 0.01;
constexpr double max_day_hours = 24.0 * 366.0;

[[nodiscard]] std::unexpected<Error> invalid(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

[[nodiscard]] Result<std::optional<double>> optional_number(const toml::table& table,
                                                            std::string_view key) {
    const auto node = table[key];
    if (!node) {
        return std::optional<double>{};
    }
    if (const auto* integer = node.as_integer()) {
        return std::optional<double>{static_cast<double>(integer->get())};
    }
    if (const auto* floating = node.as_floating_point()) {
        return std::optional<double>{floating->get()};
    }
    return invalid(std::format("planet.{} must be a number", key));
}

// NaN fails every comparison, so these reject it along with out-of-range values.
[[nodiscard]] bool positive(double value) {
    return value > 0.0 && std::isfinite(value);
}

[[nodiscard]] bool valid_ocean_fraction(double value) {
    return value >= 0.0 && value < 1.0;
}

[[nodiscard]] bool in_range(double value, double low, double high) {
    return value >= low && value <= high;
}

[[nodiscard]] Result<void> parse_calendar(const toml::table& planet, PlanetConfig& config) {
    const auto day_hours = optional_number(planet, "day_hours");
    if (!day_hours) {
        return std::unexpected(day_hours.error());
    }
    if (day_hours->has_value()) {
        if (!in_range(**day_hours, min_day_hours, max_day_hours)) {
            return invalid("planet.day_hours must be between 0.01 and a year");
        }
        config.day_length = std::chrono::round<GameDuration>(
            std::chrono::duration<double, std::ratio<3'600>>{**day_hours});
    }

    const auto tilt = optional_number(planet, "axial_tilt");
    if (!tilt) {
        return std::unexpected(tilt.error());
    }
    config.axial_tilt = tilt->value_or(config.axial_tilt);
    if (!in_range(config.axial_tilt, 0.0, 90.0)) {
        return invalid("planet.axial_tilt must be between 0 and 90 degrees");
    }
    return {};
}

} // namespace

double radius_for_population(const PopulationSizing& sizing, double ocean_fraction) {
    HOG_ASSERT(positive(sizing.population), "population must be positive");
    HOG_ASSERT(positive(sizing.settled_density), "settled density must be positive");
    HOG_ASSERT(positive(sizing.wilderness_factor), "wilderness factor must be positive");
    HOG_ASSERT(valid_ocean_fraction(ocean_fraction), "ocean fraction must be in [0, 1)");

    const double land_km2 = sizing.population / sizing.settled_density * sizing.wilderness_factor;
    const double surface_km2 = land_km2 / (1.0 - ocean_fraction);
    return std::sqrt(surface_km2 / (4.0 * std::numbers::pi)) * 1'000.0;
}

Result<PlanetConfig> parse_planet_config(std::string_view text) {
    const toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        const toml::parse_error& error = parsed.error();
        return make_error(ErrorCode::Parse, std::format("line {}: {}", error.source().begin.line,
                                                        error.description()));
    }

    const toml::table* planet = parsed.table()["planet"].as_table();
    if (planet == nullptr) {
        return invalid("missing [planet] table");
    }
    for (const auto& [key, value] : *planet) {
        if (std::ranges::find(known_keys, key.str()) == known_keys.end()) {
            return invalid(std::format("unknown key planet.{}", key.str()));
        }
    }

    PlanetConfig config;

    const auto* seed = (*planet)["seed"].as_integer();
    if (seed == nullptr || seed->get() < 0) {
        return invalid("planet.seed must be a non-negative integer");
    }
    config.seed = static_cast<std::uint64_t>(seed->get());

    const auto ocean = optional_number(*planet, "ocean_fraction");
    if (!ocean) {
        return std::unexpected(ocean.error());
    }
    config.ocean_fraction = ocean->value_or(config.ocean_fraction);
    if (!valid_ocean_fraction(config.ocean_fraction)) {
        return invalid("planet.ocean_fraction must be at least 0 and below 1");
    }

    if (auto calendar = parse_calendar(*planet, config); !calendar) {
        return std::unexpected(calendar.error());
    }

    const auto population = optional_number(*planet, "population");
    const auto density = optional_number(*planet, "settled_density");
    const auto wilderness = optional_number(*planet, "wilderness_factor");
    const auto radius_km = optional_number(*planet, "radius_km");
    for (const auto* field : {&population, &density, &wilderness, &radius_km}) {
        if (!*field) {
            return std::unexpected(field->error());
        }
    }

    if (population->has_value() == radius_km->has_value()) {
        return invalid("planet needs exactly one of population or radius_km");
    }
    if (radius_km->has_value()) {
        if (density->has_value() || wilderness->has_value()) {
            return invalid("settled_density and wilderness_factor only apply with population");
        }
        if (!positive(**radius_km)) {
            return invalid("planet.radius_km must be positive");
        }
        config.radius = **radius_km * 1'000.0;
        return config;
    }

    PopulationSizing sizing{.population = **population};
    sizing.settled_density = density->value_or(sizing.settled_density);
    sizing.wilderness_factor = wilderness->value_or(sizing.wilderness_factor);
    if (!positive(sizing.population) || !positive(sizing.settled_density) ||
        !positive(sizing.wilderness_factor)) {
        return invalid("population, settled_density and wilderness_factor must be positive");
    }
    config.radius = radius_for_population(sizing, config.ocean_fraction);
    return config;
}

Result<PlanetConfig> load_planet_config(const std::filesystem::path& path) {
    const auto text = read_text_file(path);
    if (!text) {
        return std::unexpected(text.error());
    }
    auto config = parse_planet_config(*text);
    if (!config) {
        config.error().message = std::format("{}: {}", path.string(), config.error().message);
    }
    return config;
}

std::uint64_t world_id(const PlanetConfig& config) noexcept {
    return hash_values(config.seed, std::bit_cast<std::uint64_t>(config.radius),
                       std::bit_cast<std::uint64_t>(config.ocean_fraction));
}

} // namespace hog::planet
