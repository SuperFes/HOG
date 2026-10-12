#include "toml_table.hpp"

#include <hog/core/text_file.hpp>
#include <hog/sim/ecology.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace hog::sim {

namespace {

using namespace std::string_view_literals;
using detail::extreme_temperature;
using detail::per_biome;
using detail::positive;
using detail::Table;

// The value a string node names, if it names one.
template <typename T, std::size_t N>
std::optional<T> named(const toml::node& node,
                       const std::array<std::pair<std::string_view, T>, N>& names) {
    const auto* text = node.as_string();
    if (text == nullptr) {
        return std::nullopt;
    }
    for (const auto& [name, value] : names) {
        if (name == text->get()) {
            return value;
        }
    }
    return std::nullopt;
}

void parse_forage(Table forage, ForageConfig& config) {
    forage.only({"growth_days"sv, "wither_days"sv, "cold"sv, "warm"sv, "wet"sv, "rot_days"sv,
                 "capacity"sv});
    forage.days("growth_days", config.growth);
    forage.days("wither_days", config.wither);
    forage.number("cold", -extreme_temperature, extreme_temperature, config.cold);
    forage.number("warm", -extreme_temperature, extreme_temperature, config.warm);
    if (config.warm <= config.cold) {
        forage.fail(std::format("{} must be above {}", forage.where("warm"), forage.where("cold")));
    }
    forage.number("wet", positive, 1'000.0, config.wet);
    forage.days("rot_days", config.rot);
    per_biome(forage, "capacity", 1e9, config.capacity);
}

void parse_moves(Table& table, Species& species) {
    const toml::node* node = table.find("moves");
    if (node == nullptr) {
        return;
    }
    constexpr std::array names{std::pair{"land"sv, Moves::Land}, std::pair{"water"sv, Moves::Water},
                               std::pair{"air"sv, Moves::Air}};
    const std::optional<Moves> moves = named(*node, names);
    if (!moves) {
        table.fail(std::format("{} must be land, water or air", table.where("moves")));
        return;
    }
    species.moves = *moves;
}

void parse_comfort(Table& table, Species& species) {
    const toml::node* node = table.find("comfort");
    if (node == nullptr) {
        return;
    }
    const auto* range = node->as_array();
    if (range == nullptr || range->size() != 2) {
        table.fail(std::format("{} must be [cold, heat]", table.where("comfort")));
        return;
    }
    table.number(*range->get(0), table.where("comfort"), -extreme_temperature, extreme_temperature,
                 species.cold);
    table.number(*range->get(1), table.where("comfort"), -extreme_temperature, extreme_temperature,
                 species.heat);
    if (species.heat <= species.cold) {
        table.fail(std::format("{} must rise from cold to heat", table.where("comfort")));
    }
}

void parse_timing(Table& table, Species& species) {
    Table breeding = table.child("breeding");
    breeding.only({"start_day"sv, "days"sv});
    breeding.number("start_day", 0.0, days_per_year, species.breeding.start);
    breeding.number("days", positive, days_per_year, species.breeding.days);

    Table cycle = table.child("cycle");
    cycle.only({"years"sv, "boom"sv, "width_years"sv, "region_level"sv});
    cycle.number("years", 0.0, 1'000.0, species.cycle.years);
    cycle.number("boom", 0.0, 1'000.0, species.cycle.boom);
    cycle.number("width_years", positive, 1'000.0, species.cycle.width);
    double level = species.cycle.region_level;
    cycle.number("region_level", 0.0, 10.0, level);
    species.cycle.region_level = static_cast<int>(level);
    if (species.cycle.years > 0.0 && species.cycle.width > species.cycle.years) {
        cycle.fail(std::format("{} must be at most the cycle's years", cycle.where("width_years")));
    }

    if (table.find("dormant_below") != nullptr) {
        double below = 0.0;
        table.number("dormant_below", -extreme_temperature, extreme_temperature, below);
        species.dormant_below = below;
    }
}

// Reads [earliest, latest] years.
void parse_years(Table& table, std::string_view key, double& earliest, double& latest) {
    const toml::node* node = table.find(key);
    if (node == nullptr) {
        return;
    }
    const auto* range = node->as_array();
    if (range == nullptr || range->size() != 2) {
        table.fail(std::format("{} must be [earliest, latest]", table.where(key)));
        return;
    }
    table.number(*range->get(0), table.where(key), 0.0, 10'000.0, earliest);
    table.number(*range->get(1), table.where(key), 0.0, 10'000.0, latest);
    if (latest < earliest) {
        table.fail(std::format("{} must not end before it starts", table.where(key)));
    }
}

// Successors name other species, so the name is resolved once every species is known.
std::string parse_recovery(Table& parent, Recovery& recovery) {
    Table table = parent.child("recovery");
    table.only({"years"sv, "founders"sv, "after_extinction"sv, "extinct_years"sv, "successor"sv});
    parse_years(table, "years", recovery.earliest, recovery.latest);
    table.number("founders", 0.0, 1e9, recovery.founders);
    parse_years(table, "extinct_years", recovery.extinct_earliest, recovery.extinct_latest);
    if (const toml::node* node = table.find("after_extinction")) {
        constexpr std::array names{std::pair{"admin"sv, Recovery::After::Admin},
                                   std::pair{"relict"sv, Recovery::After::Relict},
                                   std::pair{"successor"sv, Recovery::After::Successor}};
        if (const std::optional<Recovery::After> after = named(*node, names)) {
            recovery.after_extinction = *after;
        } else {
            table.fail(std::format("{} must be admin, relict or successor",
                                   table.where("after_extinction")));
        }
    }
    const toml::node* successor = table.find("successor");
    if (successor != nullptr && !successor->is_string()) {
        table.fail(std::format("{} must be a species' name", table.where("successor")));
        return {};
    }
    return successor == nullptr ? std::string{} : std::string{successor->as_string()->get()};
}

void parse_events(Table& parent, Events& events) {
    Table table = parent.child("events");
    table.only({"baseline_years"sv, "collapse"sv, "overpopulation"sv, "least"sv});
    table.number("baseline_years", positive, 10'000.0, events.baseline_years);
    table.number("collapse", positive, 0.99, events.collapse);
    table.number("overpopulation", 1.01, 1e6, events.overpopulation);
    table.number("least", 0.0, 1e9, events.least);
}

// Diets and successors name other species, so they're read once every species is known.
struct Names {
    std::vector<std::pair<std::string, double>> diet;
    std::string successor;
};

Species parse_species(Table table, Names& names) {
    table.only({"name"sv,     "moves"sv,          "mass"sv,      "appetite"sv,     "half_fed"sv,
                "diet"sv,     "habitat"sv,        "density"sv,   "crowded"sv,      "births"sv,
                "breeding"sv, "lifespan_years"sv, "starving"sv,  "hungry_below"sv, "comfort"sv,
                "exposure"sv, "dormant_below"sv,  "wander_km"sv, "speed_km"sv,     "leftovers"sv,
                "cycle"sv,    "recovery"sv,       "events"sv});
    Species species;
    const toml::node* name = table.find("name");
    if (name == nullptr || !name->is_string() || name->as_string()->get().empty()) {
        table.fail(std::format("{} must be a non-empty string", table.where("name")));
    } else {
        species.name = name->as_string()->get();
    }
    parse_moves(table, species);
    table.number("mass", positive, 1e9, species.mass);
    table.number("appetite", 0.0, 1e9, species.appetite);
    table.number("half_fed", positive, 1e12, species.half_fed);
    per_biome(table, "habitat", 1.0, species.habitat);
    table.number("density", 0.0, 1e9, species.density);
    table.number("crowded", positive, 1e9, species.crowded);
    table.number("births", 0.0, 1'000.0, species.births);
    table.number("lifespan_years", positive, 10'000.0, species.lifespan);
    table.number("starving", 0.0, 1.0, species.starving);
    table.number("hungry_below", positive, 1.0, species.hungry_below);
    parse_comfort(table, species);
    table.number("exposure", 0.0, 1.0, species.exposure);
    table.number("wander_km", 0.0, 1e4, species.wander);
    table.number("speed_km", 0.0, 1e4, species.speed);
    table.number("leftovers", 0.0, 0.99, species.leftovers);
    parse_timing(table, species);
    names.successor = parse_recovery(table, species.recovery);
    parse_events(table, species.events);

    Table foods = table.child("diet");
    if (foods.get() != nullptr) {
        for (const auto& [food, node] : *foods.get()) {
            double preference = 1.0;
            foods.number(node, foods.where(food.str()), positive, 1'000.0, preference);
            names.diet.emplace_back(food.str(), preference);
        }
    }
    return species;
}

void resolve_successor(const Ecology& ecology, const std::string& name, Species& species,
                       Table& table) {
    const bool wanted = species.recovery.after_extinction == Recovery::After::Successor;
    if (name.empty()) {
        if (wanted) {
            table.fail(std::format("{} needs a successor to take its place", species.name));
        }
        return;
    }
    if (!wanted) {
        table.fail(
            std::format("{} names a successor but isn't succeeded after_extinction", species.name));
        return;
    }
    const auto successor = ecology.find(name);
    if (!successor || *successor == ecology.find(species.name)) {
        table.fail(std::format("{}'s successor {} must be another species", species.name, name));
        return;
    }
    species.recovery.successor = *successor;
}

void resolve_names(const Ecology& ecology, const Names& names, Species& species, Table& table) {
    resolve_successor(ecology, names.successor, species, table);
    for (const auto& [name, preference] : names.diet) {
        Food food{.preference = preference};
        if (name == "forage") {
            food.kind = Food::Kind::Forage;
        } else if (name == "carrion") {
            food.kind = Food::Kind::Carrion;
        } else if (const auto prey = ecology.find(name)) {
            food.kind = Food::Kind::Prey;
            food.prey = *prey;
        } else {
            table.fail(std::format("{} eats {}, which is neither forage, carrion nor a species",
                                   species.name, name));
            continue;
        }
        species.diet.push_back(food);
    }
}

Result<Ecology> parse(const toml::table& root) {
    std::optional<Error> error;
    Table top{&root, "ecology", &error};
    top.only({"forage"sv, "species"sv});
    Ecology ecology;
    parse_forage(Table{top.table("forage"), "forage", &error}, ecology.forage);

    std::vector<Names> names;
    if (const toml::node* list = top.find("species")) {
        const auto* species = list->as_array();
        if (species == nullptr || !species->is_array_of_tables()) {
            top.fail("species must be an array of tables, as [[species]]");
        } else {
            for (const toml::node& node : *species) {
                ecology.species.push_back(
                    parse_species(Table{node.as_table(),
                                        std::format("species[{}]", ecology.species.size()), &error},
                                  names.emplace_back()));
            }
        }
    }
    for (std::size_t index = 0; index < ecology.species.size(); ++index) {
        const std::string& name = ecology.species[index].name;
        if (name == "forage" || name == "carrion") {
            top.fail(std::format("a species can't be named {}", name));
        } else if (ecology.find(name) != index) {
            top.fail(std::format("species {} is listed twice", name));
        }
        resolve_names(ecology, names[index], ecology.species[index], top);
    }
    if (error) {
        return std::unexpected(std::move(*error));
    }
    return ecology;
}

} // namespace

std::optional<std::uint32_t> Ecology::find(std::string_view name) const {
    const auto found = std::ranges::find(species, name, &Species::name);
    if (found == species.end()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(found - species.begin());
}

Result<Ecology> parse_ecology(std::string_view text) {
    const toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        const toml::parse_error& error = parsed.error();
        return make_error(ErrorCode::Parse, std::format("line {}: {}", error.source().begin.line,
                                                        error.description()));
    }
    return parse(parsed.table());
}

Result<Ecology> load_ecology(const std::filesystem::path& path) {
    auto text = read_text_file(path);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    return parse_ecology(*text);
}

} // namespace hog::sim
