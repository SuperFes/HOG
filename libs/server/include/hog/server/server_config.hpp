#pragma once

#include <hog/core/log.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/server/tick_clock.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hog::server {

struct LogConfig {
    log::Level level = log::Level::Info;
    // Overrides by category name.
    std::vector<std::pair<std::string, log::Level>> categories;
    // JSON lines, in addition to text on the console.
    std::optional<std::filesystem::path> file;
};

struct WeatherSettings {
    double cell_size = 6'000.0; // at most this many metres across a grid cell on the planet
    GameDuration step = std::chrono::minutes{10};
    // The day the weather follows, which needn't be the sun's.
    GameDuration day = std::chrono::hours{24};
};

struct WildlifeSettings {
    // Species and forage; without one, no wildlife lives on the planet.
    std::optional<std::filesystem::path> ecology;
    GameDuration step = std::chrono::hours{24};
};

struct ForestSettings {
    // How plants grow and burn; without one, nothing grows on the planet.
    std::optional<std::filesystem::path> vegetation;
    GameDuration step = std::chrono::hours{1};
};

struct ServerConfig {
    std::filesystem::path world; // the world database
    std::filesystem::path planet;
    std::uint32_t tick_rate = idle_rate;
    std::uint32_t max_burst = 5;
    GameDuration save_interval = std::chrono::minutes{5};
    std::size_t keep_snapshots = 3;
    // About how many ticks catching up to now may take, however long the server was down.
    std::uint64_t catch_up_ticks = 2'000;
    LogConfig log;
    // Real time between metrics records; zero turns them off.
    std::chrono::seconds metrics_interval{10};
    WeatherSettings weather;
    WildlifeSettings wildlife;
    ForestSettings forest;
};

// Reads a server config. Relative paths are resolved against `base`, normally the directory of
// the config file:
//
//     [server]
//     world = "../var/world.db"
//     planet = "planet.toml"
//     tick_rate = 20             # optional, hertz
//     max_burst = 5              # optional
//
//     [save]                     # optional
//     interval_minutes = 5.0     # game time
//     keep = 3
//     catch_up_ticks = 2000
//
//     [log]                      # optional
//     level = "info"
//     file = "../var/logs/server.jsonl"
//     categories = { save = "debug" }
//
//     [metrics]                  # optional
//     interval_seconds = 10      # 0 turns metrics off
//
//     [weather]                  # optional
//     cell_km = 6.0
//     step_minutes = 10.0        # game time
//     day_hours = 24.0
//
//     [wildlife]                 # optional
//     ecology = "ecology.toml"
//     step_hours = 24.0          # game time
//
//     [forest]                   # optional
//     vegetation = "forest.toml"
//     step_hours = 1.0           # game time
//
// Unknown tables and keys are errors, so typos don't silently fall back to defaults.
[[nodiscard]] Result<ServerConfig> parse_server_config(std::string_view text,
                                                       const std::filesystem::path& base);
[[nodiscard]] Result<ServerConfig> load_server_config(const std::filesystem::path& path);

} // namespace hog::server
