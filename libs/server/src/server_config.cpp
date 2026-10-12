#include <hog/core/text_file.hpp>
#include <hog/server/server_config.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>
#include <string>
#include <utility>

namespace hog::server {

namespace {

constexpr double max_save_minutes = 24.0 * 60.0;
constexpr std::int64_t max_burst_limit = 1'000;
constexpr std::int64_t max_keep = 1'000;
constexpr std::int64_t max_catch_up_ticks = 1'000'000'000;
constexpr std::int64_t max_metrics_seconds = std::int64_t{24} * 60 * 60;
constexpr double max_cell_km = 10'000.0;
constexpr double max_weather_minutes = 24.0 * 60.0;
constexpr double max_weather_day_hours = 24.0 * 365.0;
constexpr double max_wildlife_step_hours = 24.0 * 365.0;

[[nodiscard]] std::unexpected<Error> invalid(std::string message) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

// Reads one table's keys, rejecting any it doesn't know.
class Table {
public:
    Table(const toml::table* table, std::string_view name) : table_(table), name_(name) {}

    [[nodiscard]] Result<void> only(std::initializer_list<std::string_view> known) const {
        if (table_ == nullptr) {
            return {};
        }
        for (const auto& [key, value] : *table_) {
            if (std::ranges::find(known, key.str()) == known.end()) {
                return invalid(std::format("unknown key {}.{}", name_, key.str()));
            }
        }
        return {};
    }

    [[nodiscard]] Result<std::optional<std::int64_t>>
    integer(std::string_view key, std::int64_t low, std::int64_t high) const {
        const toml::node* node = find(key);
        if (node == nullptr) {
            return std::nullopt;
        }
        const auto* value = node->as_integer();
        if (value == nullptr || value->get() < low || value->get() > high) {
            return invalid(
                std::format("{}.{} must be an integer from {} to {}", name_, key, low, high));
        }
        return value->get();
    }

    [[nodiscard]] Result<std::optional<double>> number(std::string_view key) const {
        const toml::node* node = find(key);
        if (node == nullptr) {
            return std::nullopt;
        }
        if (const auto* integer = node->as_integer()) {
            return static_cast<double>(integer->get());
        }
        if (const auto* floating = node->as_floating_point()) {
            return floating->get();
        }
        return invalid(std::format("{}.{} must be a number", name_, key));
    }

    // A positive game time in units of Period, at most `max` of them.
    template <typename Period>
    [[nodiscard]] Result<std::optional<GameDuration>> duration(std::string_view key, double max,
                                                               std::string_view limit) const {
        const auto value = number(key);
        if (!value || !*value) {
            return value ? Result<std::optional<GameDuration>>{std::nullopt}
                         : std::unexpected(value.error());
        }
        // Checked before converting, which is undefined for NaN; NaN fails every comparison.
        const bool in_range = **value > 0.0 && **value <= max;
        const auto converted =
            in_range
                ? std::chrono::round<GameDuration>(std::chrono::duration<double, Period>{**value})
                : GameDuration::zero();
        if (converted <= GameDuration::zero()) {
            return invalid(std::format("{}.{} must be above 0 and at most {}", name_, key, limit));
        }
        return converted;
    }

    [[nodiscard]] Result<std::optional<std::string>> string(std::string_view key) const {
        const toml::node* node = find(key);
        if (node == nullptr) {
            return std::nullopt;
        }
        const auto* value = node->as_string();
        if (value == nullptr || value->get().empty()) {
            return invalid(std::format("{}.{} must be a non-empty string", name_, key));
        }
        return value->get();
    }

    [[nodiscard]] Result<const toml::table*> table(std::string_view key) const {
        const toml::node* node = find(key);
        if (node == nullptr) {
            return nullptr;
        }
        if (!node->is_table()) {
            return invalid(std::format("{}.{} must be a table", name_, key));
        }
        return node->as_table();
    }

    [[nodiscard]] const toml::node* find(std::string_view key) const {
        return table_ == nullptr ? nullptr : table_->get(key);
    }

private:
    const toml::table* table_;
    std::string_view name_;
};

[[nodiscard]] Result<log::Level> level(std::string_view where, const toml::node& node) {
    const auto* name = node.as_string();
    const auto parsed = name == nullptr ? std::nullopt : log::parse_level(name->get());
    if (!parsed) {
        return invalid(
            std::format("{} must be one of trace, debug, info, warn, error, off", where));
    }
    return *parsed;
}

[[nodiscard]] Result<void> parse_server(const Table& server, const std::filesystem::path& base,
                                        ServerConfig& config) {
    if (auto known = server.only({"world", "planet", "tick_rate", "max_burst"}); !known) {
        return known;
    }
    for (auto [key, path] : {std::pair{"world", &config.world}, {"planet", &config.planet}}) {
        const auto value = server.string(key);
        if (!value) {
            return std::unexpected(value.error());
        }
        if (!*value) {
            return invalid(std::format("server.{} is required", key));
        }
        *path = (base / **value).lexically_normal();
    }
    const auto rate = server.integer("tick_rate", 1, max_rate);
    const auto burst = server.integer("max_burst", 1, max_burst_limit);
    for (const auto* field : {&rate, &burst}) {
        if (!*field) {
            return std::unexpected(field->error());
        }
    }
    config.tick_rate = static_cast<std::uint32_t>(rate->value_or(config.tick_rate));
    config.max_burst = static_cast<std::uint32_t>(burst->value_or(config.max_burst));
    return {};
}

[[nodiscard]] Result<void> parse_save(const Table& save, ServerConfig& config) {
    if (auto known = save.only({"interval_minutes", "keep", "catch_up_ticks"}); !known) {
        return known;
    }
    const auto interval =
        save.duration<std::ratio<60>>("interval_minutes", max_save_minutes, "a day");
    if (!interval) {
        return std::unexpected(interval.error());
    }
    config.save_interval = interval->value_or(config.save_interval);
    const auto keep = save.integer("keep", 1, max_keep);
    const auto catch_up = save.integer("catch_up_ticks", 1, max_catch_up_ticks);
    for (const auto* field : {&keep, &catch_up}) {
        if (!*field) {
            return std::unexpected(field->error());
        }
    }
    config.keep_snapshots =
        static_cast<std::size_t>(keep->value_or(static_cast<std::int64_t>(config.keep_snapshots)));
    config.catch_up_ticks = static_cast<std::uint64_t>(
        catch_up->value_or(static_cast<std::int64_t>(config.catch_up_ticks)));
    return {};
}

[[nodiscard]] Result<void> parse_log(const Table& section, const std::filesystem::path& base,
                                     LogConfig& config) {
    if (auto known = section.only({"level", "file", "categories"}); !known) {
        return known;
    }
    if (const toml::node* node = section.find("level")) {
        const auto parsed = level("log.level", *node);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        config.level = *parsed;
    }
    const auto file = section.string("file");
    if (!file) {
        return std::unexpected(file.error());
    }
    if (*file) {
        config.file = (base / **file).lexically_normal();
    }
    const auto categories = section.table("categories");
    if (!categories) {
        return std::unexpected(categories.error());
    }
    if (*categories != nullptr) {
        for (const auto& [name, node] : **categories) {
            const auto parsed = level(std::format("log.categories.{}", name.str()), node);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            config.categories.emplace_back(std::string{name.str()}, *parsed);
        }
    }
    return {};
}

[[nodiscard]] Result<void> parse_metrics(const Table& metrics, ServerConfig& config) {
    if (auto known = metrics.only({"interval_seconds"}); !known) {
        return known;
    }
    const auto seconds = metrics.integer("interval_seconds", 0, max_metrics_seconds);
    if (!seconds) {
        return std::unexpected(seconds.error());
    }
    config.metrics_interval =
        std::chrono::seconds{seconds->value_or(config.metrics_interval.count())};
    return {};
}

[[nodiscard]] Result<void> parse_weather(const Table& weather, WeatherSettings& config) {
    if (auto known = weather.only({"cell_km", "step_minutes", "day_hours"}); !known) {
        return known;
    }
    const auto cell = weather.number("cell_km");
    if (!cell) {
        return std::unexpected(cell.error());
    }
    if (*cell) {
        // NaN fails every comparison, so it is out of range too.
        const bool in_range = **cell > 0.0 && **cell <= max_cell_km;
        if (!in_range) {
            return invalid(
                std::format("weather.cell_km must be above 0 and at most {}", max_cell_km));
        }
        config.cell_size = **cell * 1'000.0;
    }
    const auto step =
        weather.duration<std::ratio<60>>("step_minutes", max_weather_minutes, "a day");
    const auto day =
        weather.duration<std::ratio<3'600>>("day_hours", max_weather_day_hours, "a year");
    for (const auto* field : {&step, &day}) {
        if (!*field) {
            return std::unexpected(field->error());
        }
    }
    config.step = step->value_or(config.step);
    config.day = day->value_or(config.day);
    return {};
}

[[nodiscard]] Result<void> parse_wildlife(const Table& wildlife, const std::filesystem::path& base,
                                          WildlifeSettings& config) {
    if (auto known = wildlife.only({"ecology", "step_hours"}); !known) {
        return known;
    }
    const auto ecology = wildlife.string("ecology");
    if (!ecology) {
        return std::unexpected(ecology.error());
    }
    if (*ecology) {
        config.ecology = (base / **ecology).lexically_normal();
    }
    const auto step =
        wildlife.duration<std::ratio<3'600>>("step_hours", max_wildlife_step_hours, "a year");
    if (!step) {
        return std::unexpected(step.error());
    }
    config.step = step->value_or(config.step);
    return {};
}

} // namespace

Result<ServerConfig> parse_server_config(std::string_view text, const std::filesystem::path& base) {
    const toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        const toml::parse_error& error = parsed.error();
        return make_error(ErrorCode::Parse, std::format("line {}: {}", error.source().begin.line,
                                                        error.description()));
    }
    constexpr std::array<std::string_view, 6> names{"server",  "save",    "log",
                                                    "metrics", "weather", "wildlife"};
    std::array<const toml::table*, names.size()> tables{};
    for (const auto& [key, node] : parsed.table()) {
        const auto index =
            static_cast<std::size_t>(std::ranges::find(names, key.str()) - names.begin());
        if (index == names.size() || !node.is_table()) {
            return invalid(std::format("unknown table [{}]", key.str()));
        }
        tables.at(index) = node.as_table();
    }
    if (tables[0] == nullptr) {
        return invalid("missing [server] table");
    }

    ServerConfig config;
    if (auto server = parse_server(Table{tables[0], "server"}, base, config); !server) {
        return std::unexpected(server.error());
    }
    if (auto save = parse_save(Table{tables[1], "save"}, config); !save) {
        return std::unexpected(save.error());
    }
    if (auto logging = parse_log(Table{tables[2], "log"}, base, config.log); !logging) {
        return std::unexpected(logging.error());
    }
    if (auto metrics = parse_metrics(Table{tables[3], "metrics"}, config); !metrics) {
        return std::unexpected(metrics.error());
    }
    if (auto weather = parse_weather(Table{tables[4], "weather"}, config.weather); !weather) {
        return std::unexpected(weather.error());
    }
    if (auto wildlife = parse_wildlife(Table{tables[5], "wildlife"}, base, config.wildlife);
        !wildlife) {
        return std::unexpected(wildlife.error());
    }
    return config;
}

Result<ServerConfig> load_server_config(const std::filesystem::path& path) {
    const auto text = read_text_file(path);
    if (!text) {
        return std::unexpected(text.error());
    }
    auto config = parse_server_config(*text, path.parent_path());
    if (!config) {
        config.error().message = std::format("{}: {}", path.string(), config.error().message);
    }
    return config;
}

} // namespace hog::server
