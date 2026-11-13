#include <hog/client/play.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

// Draws a planet it builds itself, alone: for looking and testing.
int main(int argc, char** argv) {
    const std::span raw{argv, static_cast<std::size_t>(argc)};
    const std::vector<std::string_view> arguments(raw.begin() + 1, raw.end());
    return hog::client::play(arguments, hog::client::Mode::Viewer);
}
