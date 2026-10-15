#pragma once

#include <hog/core/build_info.hpp>
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
    // The local time of day to start at, from 0 at midnight to 1; the planet's own without one.
    std::optional<double> time_of_day;
    std::uint32_t threads = 0; // baking the ground, 0 for one per hardware thread
    // A drawing on white paper, of someone to fly along with; none without one.
    std::optional<std::filesystem::path> companion = std::filesystem::path{"res/Matt.png"};
    // For smoke tests and pictures: how long to run before quitting, and where to save the last
    // frame drawn, as a PNG.
    std::optional<std::chrono::milliseconds> quit_after;
    std::optional<std::filesystem::path> capture;
    // A software Vulkan device over a GPU, and the validation layer with its errors failing the
    // run, also for tests. Debug builds validate unless told not to.
    bool software = false;
    bool validate = debug_build;
    bool help = false;
};

[[nodiscard]] std::string_view usage() noexcept;

// Parses the arguments after the program name.
[[nodiscard]] Result<Options> parse_options(std::span<const std::string_view> arguments);

// `path` if it's absolute or found from the working directory, else found from `from` or the
// nearest directory above it, such as the program's; as given if neither finds it.
[[nodiscard]] std::filesystem::path locate(const std::filesystem::path& path,
                                           const std::filesystem::path& from);

} // namespace hog::client
