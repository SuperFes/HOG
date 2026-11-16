#pragma once

#include <span>
#include <string_view>

namespace hog::client {

// Opens the window and plays until it closes, `arguments` being the command line after the
// program's name. Returns the program's exit status.
[[nodiscard]] int play(std::span<const std::string_view> arguments);

} // namespace hog::client
