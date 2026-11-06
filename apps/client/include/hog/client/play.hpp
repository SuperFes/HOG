#pragma once

#include <span>
#include <string_view>

namespace hog::client {

// Which program is playing: the viewer builds a planet itself and draws it alone, for looking and
// testing; the client only connects to a server and draws its world.
enum class Mode { Viewer, Client };

// Opens the window and plays until it closes, `arguments` being the command line after the
// program's name. Returns the program's exit status.
[[nodiscard]] int play(std::span<const std::string_view> arguments, Mode mode);

} // namespace hog::client
