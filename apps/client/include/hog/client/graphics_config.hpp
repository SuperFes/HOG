#pragma once

#include <hog/client/ground.hpp>
#include <hog/core/result.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

namespace hog::client {

// What this machine draws, for what its GPU can afford, as config/graphics.toml has it.
struct GraphicsConfig {
    GrassBudget plants;

    friend bool operator==(const GraphicsConfig&, const GraphicsConfig&) = default;
};

// Every key may be left out, keeping its default.
[[nodiscard]] Result<GraphicsConfig> parse_graphics_config(std::string_view text);
[[nodiscard]] Result<GraphicsConfig> load_graphics_config(const std::filesystem::path& path);

// The config at `asked`, found as client::locate finds it from `from`; without one,
// config/graphics.toml if this machine keeps one, else the defaults.
[[nodiscard]] Result<GraphicsConfig>
find_graphics_config(const std::optional<std::filesystem::path>& asked,
                     const std::filesystem::path& from);

} // namespace hog::client
