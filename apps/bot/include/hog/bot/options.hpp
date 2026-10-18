#pragma once

#include <hog/core/result.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace hog::bot {

struct Options {
    std::filesystem::path planet = "config/planet.toml";
    // The server's IP address and port.
    std::string connect;
    // A login service to get connect tokens from; connecting with none without one.
    std::optional<std::string> login;
    // Bots log in as name-1, name-2 and so on.
    std::string name = "bot";
    std::uint32_t count = 1;
    std::uint64_t seed = 1;
    // How long to run; forever without one.
    std::optional<std::chrono::seconds> run_for;
    bool help = false;
};

[[nodiscard]] std::string_view usage() noexcept;

// Parses the arguments after the program name.
[[nodiscard]] Result<Options> parse_options(std::span<const std::string_view> arguments);

} // namespace hog::bot
