#include <hog/client/play.hpp>

#include <string_view>
#include <vector>

// Draws a planet it builds itself, alone: for looking and testing.
int main(int argc, char** argv) {
    const std::vector<std::string_view> arguments(argv + 1, argv + argc);
    return hog::client::play(arguments, hog::client::Mode::Viewer);
}
