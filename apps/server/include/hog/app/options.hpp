#pragma once

#include <hog/core/result.hpp>

#include <filesystem>
#include <span>
#include <string_view>

namespace hog::app {

struct Options {
    std::filesystem::path config = "config/server.toml";
    bool help = false;
};

[[nodiscard]] std::string_view usage() noexcept;

// Parses the arguments after the program name.
[[nodiscard]] Result<Options> parse_options(std::span<const std::string_view> arguments);

} // namespace hog::app
