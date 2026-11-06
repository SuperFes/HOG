#include <hog/client/play.hpp>

#include <string_view>
#include <vector>

// The game client: connects to a server and draws its world.
int main(int argc, char** argv) {
    const std::vector<std::string_view> arguments(argv + 1, argv + argc);
    return hog::client::play(arguments, hog::client::Mode::Client);
}
