#pragma once

#include <hog/core/result.hpp>
#include <hog/viewer/overlay.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace hog::viewer {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 7777;
    int width = 1'024; // of the rendered map, in pixels
    Overlay overlay = Overlay::Terrain;
    std::string species; // shown first, when the overlay is wildlife
    bool globe = false;  // shown first rather than the map
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

} // namespace hog::viewer
