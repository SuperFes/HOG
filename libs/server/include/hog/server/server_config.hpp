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
// Unknown tables and keys are errors, so typos don't silently fall back to defaults.
[[nodiscard]] Result<ServerConfig> parse_server_config(std::string_view text,
                                                       const std::filesystem::path& base);
[[nodiscard]] Result<ServerConfig> load_server_config(const std::filesystem::path& path);

} // namespace hog::server
