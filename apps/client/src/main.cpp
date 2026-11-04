#include <hog/client/options.hpp>

#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace client = hog::client;

// The game client: it draws the world its server sends and builds none of its own. Until the
// server streams the ground, `hog-viewer` is what draws a planet.
int run(std::span<const std::string_view> arguments) {
    auto options = client::parse_options(arguments);
    if (!options) {
        std::cerr << std::format("hog-client: {}\n\n{}", options.error().message, client::usage());
        return EXIT_FAILURE;
    }
    if (options->help) {
        std::cout << client::usage();
        return EXIT_SUCCESS;
    }
    if (!options->connect) {
        std::cerr << "hog-client: --connect ADDRESS is required; hog-viewer draws a planet alone\n";
        return EXIT_FAILURE;
    }
    std::cerr << "hog-client: the server doesn't stream the world yet\n";
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span<char*> given{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(given.begin() + 1, given.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-client: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
