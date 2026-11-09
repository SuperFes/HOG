#include "planet_knobs.hpp"

#include <hog/core/text_file.hpp>
#include <hog/globe/planet.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <string>
#include <utility>

namespace hog::globe {
namespace {

std::unexpected<Error> invalid(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

template <typename Table>
bool listed(const Table& table, std::string_view key) {
    return std::ranges::find(table, key, &Table::value_type::first) != table.end();
}

bool known(std::string_view key) {
    return key == "seed" || key == "ore_abundance" || listed(knobs::body_numbers, key) ||
           listed(knobs::body_counts, key) || listed(knobs::shell, key) || planet::system_key(key);
}

Result<void> read_number(const toml::node_view<const toml::node> node, std::string_view key,
                         double& into) {
    if (!node) {
        return {};
    }
    if (const auto* integer = node.as_integer()) {
        into = static_cast<double>(integer->get());
        return {};
    }
    if (const auto* floating = node.as_floating_point()) {
        into = floating->get();
        return {};
    }
    return invalid(std::format("planet.{} must be a number", key));
}

Result<void> read_count(const toml::node_view<const toml::node> node, std::string_view key,
                        int& into) {
    if (!node) {
        return {};
    }
    const auto* integer = node.as_integer();
    if (integer == nullptr || integer->get() < 0 ||
        integer->get() > std::numeric_limits<int>::max()) {
        return invalid(std::format("planet.{} must be a whole number, 0 or more", key));
    }
    into = static_cast<int>(integer->get());
    return {};
}

Result<void> read_ores(const toml::table& table, formation::Body::OreAbundance& into) {
    const auto node = table["ore_abundance"];
    if (!node) {
        return {};
    }
    const toml::table* ores = node.as_table();
    if (ores == nullptr) {
        return invalid("planet.ore_abundance must be a table");
    }
    for (const auto& [key, value] : *ores) {
        if (!listed(knobs::ores, key.str())) {
            return invalid(std::format("unknown key planet.ore_abundance.{}", key.str()));
        }
    }
    for (const auto& [key, member] : knobs::ores) {
        if (auto read =
                read_number((*ores)[key], std::format("ore_abundance.{}", key), into.*member);
            !read) {
            return read;
        }
    }
    return {};
}

Result<void> read_knobs(const toml::table& table, Planet& planet) {
    if (const auto node = table["seed"]) {
        const auto* seed = node.as_integer();
        if (seed == nullptr || seed->get() < 0) {
            return invalid("planet.seed must be a non-negative integer");
        }
        planet.body.seed = static_cast<std::uint64_t>(seed->get());
    }
    for (const auto& [key, member] : knobs::body_numbers) {
        if (auto read = read_number(table[key], key, planet.body.*member); !read) {
            return read;
        }
    }
    for (const auto& [key, member] : knobs::body_counts) {
        if (auto read = read_count(table[key], key, planet.body.*member); !read) {
            return read;
        }
    }
    for (const auto& [key, member] : knobs::shell) {
        if (auto read = read_number(table[key], key, planet.*member); !read) {
            return read;
        }
    }
    return read_ores(table, planet.body.ore_abundance);
}

} // namespace

Result<Planet> parse_planet(std::string_view text) {
    const toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        const toml::parse_error& error = parsed.error();
        return make_error(ErrorCode::Parse, std::format("line {}: {}", error.source().begin.line,
                                                        error.description()));
    }
    for (const auto& [key, value] : parsed.table()) {
        if (key.str() != "planet") {
            return invalid(
                std::format("unknown table [{}]; a planet is all in [planet]", key.str()));
        }
    }
    const toml::table* table = parsed.table()["planet"].as_table();
    if (table == nullptr) {
        return invalid("missing [planet] table");
    }
    for (const auto& [key, value] : *table) {
        if (!known(key.str())) {
            return invalid(std::format("unknown key planet.{}", key.str()));
        }
    }

    Planet planet;
    if (auto read = read_knobs(*table, planet); !read) {
        return std::unexpected(read.error());
    }
    auto system = planet::parse_system(text);
    if (!system) {
        return std::unexpected(system.error());
    }
    planet.system = std::move(*system);
    if (auto valid = validate(planet); !valid) {
        return std::unexpected(valid.error());
    }
    return planet;
}

Result<Planet> load_planet(const std::filesystem::path& path) {
    const auto text = read_text_file(path);
    if (!text) {
        return std::unexpected(text.error());
    }
    auto planet = parse_planet(*text);
    if (!planet) {
        planet.error().message = std::format("{}: {}", path.string(), planet.error().message);
    }
    return planet;
}

} // namespace hog::globe
