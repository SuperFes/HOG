#pragma once

#include <hog/core/result.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace hog::client {

// Where the eye starts.
struct Place {
    double latitude = 0.0;  // degrees
    double longitude = 0.0; // degrees
    double altitude = 0.0;  // metres above sea level

    friend constexpr bool operator==(const Place&, const Place&) = default;
};

struct Options {
    std::filesystem::path planet = "config/planet.toml";
    // Over the starting islands without one.
    std::optional<Place> start;
    std::uint32_t threads = 0; // baking the ground, 0 for one per hardware thread
    // For smoke tests and pictures: how long to run before quitting, and where to save the last
    // frame drawn, as a PNG.
    std::optional<std::chrono::milliseconds> quit_after;
    std::optional<std::filesystem::path> capture;
    // A software Vulkan device over a GPU, and the validation layer with its errors failing the
    // run, also for tests.
    bool software = false;
    bool validate = false;
    bool help = false;
};

[[nodiscard]] std::string_view usage() noexcept;

// Parses the arguments after the program name.
[[nodiscard]] Result<Options> parse_options(std::span<const std::string_view> arguments);

} // namespace hog::client
