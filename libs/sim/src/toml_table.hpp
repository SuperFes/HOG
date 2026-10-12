#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/climate.hpp>
#include <hog/sim/ecology.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace hog::sim::detail {

inline constexpr double positive = std::numeric_limits<double>::min();
inline constexpr double extreme_temperature = 100.0;
inline constexpr double most_days = 10.0 * days_per_year;

// Reads one table's values, keeping the first failure for every table read alongside it.
class Table {
public:
    Table(const toml::table* table, std::string name, std::optional<Error>* error)
        : table_(table), name_(std::move(name)), error_(error) {}

    void only(std::initializer_list<std::string_view> known) {
        if (table_ == nullptr) {
            return;
        }
        for (const auto& [key, value] : *table_) {
            if (std::ranges::find(known, key.str()) == known.end()) {
                fail(std::format("unknown key {}.{}", name_, key.str()));
            }
        }
    }

    // Leaves `value` alone when the key is missing.
    void number(std::string_view key, double low, double high, double& value) {
        if (const toml::node* node = find(key)) {
            number(*node, where(key), low, high, value);
        }
    }

    void number(const toml::node& node, const std::string& where, double low, double high,
                double& value) {
        std::optional<double> read;
        if (const auto* integer = node.as_integer()) {
            read = static_cast<double>(integer->get());
        } else if (const auto* floating = node.as_floating_point()) {
            read = floating->get();
        }
        // Written so NaN fails.
        const bool in_range = read && *read >= low && *read <= high;
        if (!in_range) {
            fail(std::format("{} must be a number from {} to {}", where, low, high));
            return;
        }
        value = *read;
    }

    void days(std::string_view key, Seconds& value) {
        double count = value.count() / 86'400.0;
        number(key, positive, most_days, count);
        value = Seconds{count * 86'400.0};
    }

    // Null when missing, or when it isn't a table, which fails.
    [[nodiscard]] const toml::table* table(std::string_view key) {
        const toml::node* node = find(key);
        if (node != nullptr && !node->is_table()) {
            fail(std::format("{} must be a table", where(key)));
        }
        return node == nullptr ? nullptr : node->as_table();
    }

    [[nodiscard]] Table child(std::string_view key) {
        return Table{table(key), where(key), error_};
    }

    [[nodiscard]] const toml::node* find(std::string_view key) const {
        return table_ == nullptr ? nullptr : table_->get(key);
    }

    [[nodiscard]] const toml::table* get() const noexcept { return table_; }
    [[nodiscard]] std::string where(std::string_view key) const {
        return std::format("{}.{}", name_, key);
    }

    void fail(std::string message) {
        if (!*error_) {
            *error_ = Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)};
        }
    }

private:
    const toml::table* table_;
    std::string name_;
    std::optional<Error>* error_;
};

inline std::optional<planet::Biome> biome_named(std::string_view name) {
    std::string spaced{name};
    std::ranges::replace(spaced, '_', ' ');
    for (int index = 0; index < planet::biome_count; ++index) {
        const auto biome = static_cast<planet::Biome>(index);
        if (planet::to_string(biome) == spaced) {
            return biome;
        }
    }
    return std::nullopt;
}

inline void per_biome(Table& parent, std::string_view key, double high,
                      std::array<double, planet::biome_count>& values, double low = 0.0) {
    Table table = parent.child(key);
    if (table.get() == nullptr) {
        return;
    }
    for (const auto& [name, node] : *table.get()) {
        const auto biome = biome_named(name.str());
        if (!biome) {
            table.fail(std::format("unknown biome {}", table.where(name.str())));
            continue;
        }
        table.number(node, table.where(name.str()), low, high,
                     values.at(static_cast<std::size_t>(*biome)));
    }
}

} // namespace hog::sim::detail
