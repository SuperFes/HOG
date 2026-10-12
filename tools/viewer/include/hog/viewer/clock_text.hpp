#pragma once

#include <hog/core/time.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace hog::viewer {

struct Speed {
    std::uint32_t scale; // game seconds per real second
    std::string_view label;
};

// The speeds offered for a server's clock.
inline constexpr std::array speeds{
    Speed{.scale = 1, .label = "1x"}, Speed{.scale = 60, .label = "1 min/s"},
    Speed{.scale = 600, .label = "10 min/s"}, Speed{.scale = 3'600, .label = "1 h/s"},
    Speed{.scale = 86'400, .label = "1 day/s"}};

// How far a world's clock is from real time, such as "3d 4h ahead"; within a few seconds, which
// is about as old as a fetched frame gets, counts as on it.
[[nodiscard]] std::string drift_text(GameTime game, GameTime real);

} // namespace hog::viewer
