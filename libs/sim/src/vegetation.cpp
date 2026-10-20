#include "toml_table.hpp"

#include <hog/core/text_file.hpp>
#include <hog/sim/vegetation.hpp>

#include <toml++/toml.hpp>

#include <format>
#include <optional>
#include <utility>

namespace hog::sim {

namespace {

using namespace std::string_view_literals;
using detail::extreme_temperature;
using detail::positive;
using detail::Table;

void parse_growth(Table growth, GrowthConfig& config) {
    growth.only({"capacity"sv, "years"sv, "seeds"sv, "cold"sv, "warm"sv, "wet"sv});
    detail::per_biome(growth, "capacity", 1e6, config.capacity);
    detail::per_biome(growth, "years", 1e4, config.years, positive);
    growth.number("seeds", positive, 0.5, config.seeds);
    growth.number("cold", -extreme_temperature, extreme_temperature, config.cold);
    growth.number("warm", -extreme_temperature, extreme_temperature, config.warm);
    if (config.warm <= config.cold) {
        growth.fail(std::format("{} must be above {}", growth.where("warm"), growth.where("cold")));
    }
    growth.number("wet", positive, 1.0, config.wet);
}

void parse_fire(Table fire, FireConfig& config) {
    fire.only({"damp"sv, "parched"sv, "fuel"sv, "lightning"sv, "spread"sv, "wind"sv, "burn_days"sv,
               "douse"sv, "out"sv});
    fire.number("damp", 0.0, 1.0, config.damp);
    fire.number("parched", 0.0, 1.0, config.parched);
    if (config.parched <= config.damp) {
        fire.fail(std::format("{} must be above {}", fire.where("parched"), fire.where("damp")));
    }
    fire.number("fuel", positive, 1e6, config.fuel);
    fire.number("lightning", 0.0, 1e6, config.lightning);
    fire.number("spread", 0.0, 1e6, config.spread);
    fire.number("wind", 0.0, 10.0, config.wind);
    fire.number("burn_days", positive, 1e4, config.burn_days);
    fire.number("douse", positive, 1e6, config.douse);
    fire.number("out", positive, 1.0, config.out);
}

void parse_dying(Table dying, DyingConfig& config) {
    dying.only({"days"sv, "tolerance"sv, "learning"sv, "adapting"sv, "worst"sv, "years"sv,
                "standing"sv, "maturing"sv});
    dying.number("days", positive, 1e5, config.days);
    dying.number("tolerance", 0.0, 1.0, config.tolerance);
    if (config.tolerance >= 1.0) {
        dying.fail(std::format("{} must be below 1", dying.where("tolerance")));
    }
    dying.number("learning", 0.0, 1e4, config.learning);
    dying.number("adapting", positive, 1e4, config.adapting);
    dying.number("worst", 0.0, 1.0, config.worst);
    dying.number("years", positive, 1e4, config.years);
    dying.number("standing", positive, 1e4, config.standing);
    dying.number("maturing", positive, 1e4, config.maturing);
}

Result<Vegetation> parse(const toml::table& root) {
    std::optional<Error> error;
    Table top{&root, "vegetation", &error};
    top.only({"growth"sv, "fire"sv, "dying"sv});
    Vegetation vegetation;
    parse_growth(Table{top.table("growth"), "growth", &error}, vegetation.growth);
    parse_fire(Table{top.table("fire"), "fire", &error}, vegetation.fire);
    parse_dying(Table{top.table("dying"), "dying", &error}, vegetation.dying);
    if (error) {
        return std::unexpected(std::move(*error));
    }
    return vegetation;
}

} // namespace

Result<Vegetation> parse_vegetation(std::string_view text) {
    const toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        const toml::parse_error& error = parsed.error();
        return make_error(ErrorCode::Parse, std::format("line {}: {}", error.source().begin.line,
                                                        error.description()));
    }
    return parse(parsed.table());
}

Result<Vegetation> load_vegetation(const std::filesystem::path& path) {
    auto text = read_text_file(path);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    return parse_vegetation(*text);
}

} // namespace hog::sim
