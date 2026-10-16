#pragma once

#include <hog/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace hog::login_app {

// HOGS on a phone keypad.
inline constexpr std::uint16_t default_port = 4647;

struct Options {
    std::uint16_t port = default_port;
    // Where the signing key's seed, login.key, and its public key, login.pub, are kept.
    std::filesystem::path keys = "var";
    bool help = false;
};

[[nodiscard]] std::string_view usage() noexcept;

// Parses the arguments after the program name.
[[nodiscard]] Result<Options> parse_options(std::span<const std::string_view> arguments);

} // namespace hog::login_app
