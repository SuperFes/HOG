#include <hog/client/graphics_config.hpp>
#include <hog/client/options.hpp>
#include <hog/core/text_file.hpp>
#include <hog/render/grass.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace hog::client {

namespace {

[[nodiscard]] std::unexpected<Error> invalid(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

// A key, the table it's in, and the budget's field it sets.
struct Key {
    std::string_view table;
    std::string_view name;
    double GrassBudget::* field;
};

constexpr std::array keys{
    Key{.table = "grass", .name = "lawn_near", .field = &GrassBudget::lawn_near},
    Key{.table = "grass", .name = "lawn_far", .field = &GrassBudget::lawn_far},
    Key{.table = "grass", .name = "finest", .field = &GrassBudget::finest},
    Key{.table = "grass", .name = "density", .field = &GrassBudget::density},
    Key{.table = "grass", .name = "tall_coarsest", .field = &GrassBudget::tall_coarsest},
    Key{.table = "grass", .name = "tall_density", .field = &GrassBudget::tall_density},
    Key{.table = "grass", .name = "tall_hold", .field = &GrassBudget::tall_hold},
    Key{.table = "grass", .name = "tall_far", .field = &GrassBudget::tall_far},
    Key{.table = "bushes", .name = "far", .field = &GrassBudget::bush_far},
    Key{.table = "trees", .name = "far", .field = &GrassBudget::tree_far},
    Key{.table = "forest", .name = "full", .field = &GrassBudget::forest_full},
    Key{.table = "forest", .name = "mid", .field = &GrassBudget::forest_mid},
    Key{.table = "forest", .name = "far", .field = &GrassBudget::forest_far},
};

// Further than any tile pool holds, so a typo can't ask for the whole planet.
constexpr double furthest_plants = 10'000.0;

Result<void> read_table(std::string_view name, const toml::table& table, GrassBudget& budget) {
    for (const auto& [key, node] : table) {
        const auto* const known = std::ranges::find_if(keys, [&](const Key& candidate) {
            return candidate.table == name && candidate.name == key.str();
        });
        if (known == keys.end()) {
            return invalid(std::format("unknown key {}.{}", name, key.str()));
        }
        const std::optional<double> value = node.value<double>();
        if (!value || !std::isfinite(*value) || *value < 0.0) {
            return invalid(std::format("{}.{} must be a number, 0 or more", name, key.str()));
        }
        budget.*known->field = *value;
    }
    return {};
}

// Each kind's distances in order from the eye out, and the lawn's thinning over a stretch of
// ground.
Result<void> check(const GrassBudget& budget) {
    if (budget.finest <= 0.0 || budget.tall_coarsest < budget.finest ||
        budget.tall_coarsest > render::coarsest_grass_cells) {
        return invalid(std::format("grass.finest must be over 0, no more than grass.tall_coarsest, "
                                   "and that no more than {} m",
                                   render::coarsest_grass_cells));
    }
    if (budget.tall_hold <= 0.0) {
        return invalid("grass.tall_hold must be over 0");
    }
    if (budget.lawn_near >= budget.lawn_far) {
        return invalid("grass.lawn_near must be less than grass.lawn_far");
    }
    if (budget.lawn_far > budget.tall_far || budget.tall_far > render::furthest_grass) {
        return invalid(std::format("grass.lawn_far must be no more than grass.tall_far, and that "
                                   "no more than {} m",
                                   render::furthest_grass));
    }
    if (budget.bush_far > furthest_plants) {
        return invalid(std::format("bushes.far must be no more than {} m", furthest_plants));
    }
    if (budget.forest_full <= 0.0 || budget.forest_full > budget.forest_mid ||
        budget.forest_mid > budget.forest_far || budget.forest_far > budget.tree_far ||
        budget.tree_far > furthest_plants) {
        return invalid(std::format("forest.full, forest.mid, forest.far and trees.far must run "
                                   "out from over 0 to no more than {} m",
                                   furthest_plants));
    }
    return {};
}

} // namespace

Result<GraphicsConfig> parse_graphics_config(std::string_view text) {
    const toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        const toml::parse_error& error = parsed.error();
        return make_error(ErrorCode::Parse, std::format("line {}: {}", error.source().begin.line,
                                                        error.description()));
    }
    GraphicsConfig config;
    for (const auto& [key, node] : parsed.table()) {
        const std::string_view name = key.str();
        if (std::ranges::none_of(keys, [&](const Key& known) { return known.table == name; })) {
            return invalid(std::format("unknown table {}", name));
        }
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            return invalid(std::format("{} must be a table", name));
        }
        if (auto read = read_table(name, *table, config.plants); !read) {
            return std::unexpected(std::move(read.error()));
        }
    }
    if (auto checked = check(config.plants); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    return config;
}

Result<GraphicsConfig> load_graphics_config(const std::filesystem::path& path) {
    const auto text = read_text_file(path);
    if (!text) {
        return std::unexpected(text.error());
    }
    auto config = parse_graphics_config(*text);
    if (!config) {
        config.error().message = std::format("{}: {}", path.string(), config.error().message);
    }
    return config;
}

Result<GraphicsConfig> find_graphics_config(const std::optional<std::filesystem::path>& asked,
                                            const std::filesystem::path& from) {
    if (asked) {
        return load_graphics_config(locate(*asked, from));
    }
    const std::filesystem::path kept = locate("config/graphics.toml", from);
    std::error_code error;
    if (!std::filesystem::exists(kept, error)) {
        return GraphicsConfig{};
    }
    return load_graphics_config(kept);
}

} // namespace hog::client
