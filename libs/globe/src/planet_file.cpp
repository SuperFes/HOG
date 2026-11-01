#include <hog/core/text_file.hpp>
#include <hog/globe/planet.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <utility>

namespace hog::globe {
namespace {

using namespace std::string_view_literals;

std::unexpected<Error> invalid(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

constexpr std::array numbers{
    std::pair{"radius"sv, &Planet::radius},
    std::pair{"gravity"sv, &Planet::gravity},
    std::pair{"day"sv, &Planet::day},
    std::pair{"tilt"sv, &Planet::tilt},
    std::pair{"lateral_scale"sv, &Planet::lateral_scale},
    std::pair{"voxel"sv, &Planet::voxel},
    std::pair{"floor_depth"sv, &Planet::floor_depth},
    std::pair{"sky_height"sv, &Planet::sky_height},
    std::pair{"high_fraction"sv, &Planet::high_fraction},
    std::pair{"mantle_density"sv, &Planet::mantle_density},
    std::pair{"heat_flow"sv, &Planet::heat_flow},
    std::pair{"surface_age"sv, &Planet::surface_age},
    std::pair{"impact_flux"sv, &Planet::impact_flux},
    std::pair{"pressure"sv, &Planet::pressure},
    std::pair{"equator_temperature"sv, &Planet::equator_temperature},
    std::pair{"pole_temperature"sv, &Planet::pole_temperature},
    std::pair{"sea_fraction"sv, &Planet::sea_fraction},
};

constexpr std::array regimes{std::pair{"plates"sv, Regime::Plates},
                             std::pair{"stagnant_lid"sv, Regime::StagnantLid},
                             std::pair{"episodic"sv, Regime::Episodic}};
constexpr std::array layouts{std::pair{"scattered"sv, Layout::Scattered},
                             std::pair{"one_side"sv, Layout::OneSide}};
constexpr std::array volatiles{std::pair{"none"sv, Volatile::None},
                               std::pair{"water"sv, Volatile::Water},
                               std::pair{"methane"sv, Volatile::Methane}};
// Only rock that can make a crust.
constexpr std::array rocks{std::pair{"granite"sv, Material::Granite},
                           std::pair{"basalt"sv, Material::Basalt},
                           std::pair{"anorthosite"sv, Material::Anorthosite},
                           std::pair{"limestone"sv, Material::Limestone},
                           std::pair{"sandstone"sv, Material::Sandstone},
                           std::pair{"shale"sv, Material::Shale}};

constexpr std::array other_keys{"preset"sv,    "seed"sv, "regime"sv, "layout"sv,
                                "volatiles"sv, "high"sv, "low"sv};

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
    return invalid(std::format("{} must be a number", key));
}

template <typename T, std::size_t N>
Result<void> read_name(const toml::node_view<const toml::node> node, std::string_view key,
                       const std::array<std::pair<std::string_view, T>, N>& names, T& into) {
    if (!node) {
        return {};
    }
    const auto* text = node.as_string();
    const auto* found = text == nullptr ? names.end()
                                        : std::ranges::find(names, std::string_view{text->get()},
                                                            &std::pair<std::string_view, T>::first);
    if (found == names.end()) {
        return invalid(std::format("{} must be one of its names", key));
    }
    into = found->second;
    return {};
}

Result<void> read_crust(const toml::node_view<const toml::node> node, std::string_view key,
                        Crust& into) {
    if (!node) {
        return {};
    }
    const auto* table = node.as_table();
    if (table == nullptr) {
        return invalid(std::format("{} must be a table", key));
    }
    for (const auto& [name, value] : *table) {
        if (name.str() != "rock" && name.str() != "thickness") {
            return invalid(std::format("unknown key {}.{}", key, name.str()));
        }
    }
    if (auto read = read_name((*table)["rock"], std::format("{}.rock", key), rocks, into.rock);
        !read) {
        return read;
    }
    return read_number((*table)["thickness"], std::format("{}.thickness", key), into.thickness);
}

} // namespace

Result<Planet> parse_planet(std::string_view text) {
    const toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        const toml::parse_error& error = parsed.error();
        return make_error(ErrorCode::Parse, std::format("line {}: {}", error.source().begin.line,
                                                        error.description()));
    }
    const toml::table* globe = parsed.table()["globe"].as_table();
    if (globe == nullptr) {
        return invalid("missing [globe] table");
    }
    for (const auto& [key, value] : *globe) {
        const bool known =
            std::ranges::find(other_keys, key.str()) != other_keys.end() ||
            std::ranges::find(numbers, key.str(), &decltype(numbers)::value_type::first) !=
                numbers.end();
        if (!known) {
            return invalid(std::format("unknown key globe.{}", key.str()));
        }
    }

    Planet planet;
    if (const auto node = (*globe)["preset"]) {
        const auto* name = node.as_string();
        const auto chosen = name == nullptr ? std::nullopt : preset(name->get());
        if (!chosen) {
            return invalid("globe.preset must be earth, mars, venus or moon");
        }
        planet = *chosen;
    }
    if (const auto node = (*globe)["seed"]) {
        const auto* seed = node.as_integer();
        if (seed == nullptr || seed->get() < 0) {
            return invalid("globe.seed must be a non-negative integer");
        }
        planet.seed = static_cast<std::uint64_t>(seed->get());
    }
    for (const auto& [key, member] : numbers) {
        if (auto read = read_number((*globe)[key], std::format("globe.{}", key), planet.*member);
            !read) {
            return std::unexpected(read.error());
        }
    }
    for (auto read :
         {read_name((*globe)["regime"], "globe.regime", regimes, planet.regime),
          read_name((*globe)["layout"], "globe.layout", layouts, planet.layout),
          read_name((*globe)["volatiles"], "globe.volatiles", volatiles, planet.volatiles),
          read_crust((*globe)["high"], "globe.high", planet.high),
          read_crust((*globe)["low"], "globe.low", planet.low)}) {
        if (!read) {
            return std::unexpected(read.error());
        }
    }
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
    return parse_planet(*text);
}

} // namespace hog::globe
