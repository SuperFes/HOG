#pragma once

#include <hog/core/result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hog::viewer {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
    int width = 1'024; // of the rendered map, in pixels
    bool help = false;
};

[[nodiscard]] std::string_view usage() noexcept;

// Parses the arguments after the program name.
[[nodiscard]] Result<Options> parse_options(std::span<const std::string_view> arguments);

} // namespace hog::viewer
