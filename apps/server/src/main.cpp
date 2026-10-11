#include <hog/app/logging.hpp>
#include <hog/app/options.hpp>
#include <hog/app/server.hpp>
#include <hog/server/server_config.hpp>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

// Set from a signal handler, which may only touch lock-free atomics, so it has to be global.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void request_stop(int /*signal*/) {
    stop_requested.store(true, std::memory_order_relaxed);
}

hog::GameTime real_now() {
    return hog::to_game_time(hog::WallClock::now());
}

int run(std::span<const std::string_view> arguments) {
    const auto options = hog::app::parse_options(arguments);
    if (!options) {
        std::cerr << std::format("hog-server: {}\n\n{}", options.error().message,
                                 hog::app::usage());
        return EXIT_FAILURE;
    }
    if (options->help) {
        std::cout << hog::app::usage();
        return EXIT_SUCCESS;
    }
    const auto config = hog::server::load_server_config(options->config);
    if (!config) {
        std::cerr << std::format("hog-server: {}\n", config.error());
        return EXIT_FAILURE;
    }
    const auto logging = hog::app::Logging::start(config->log, std::clog);
    if (!logging) {
        std::cerr << std::format("hog-server: {}\n", logging.error());
        return EXIT_FAILURE;
    }

    const auto server = hog::app::Server::open(*config, real_now());
    if (!server) {
        hog::log::error(hog::app::server_log, "cannot start: {}", server.error());
        return EXIT_FAILURE;
    }
    for (const int signal : {SIGINT, SIGTERM}) {
        if (std::signal(signal, request_stop) == SIG_ERR) {
            hog::log::warn(hog::app::server_log, "cannot handle signal {}; stop won't save",
                           signal);
        }
    }
    (*server)->catch_up(real_now);
    hog::log::info(hog::app::server_log, "running at {} Hz from tick {}", config->tick_rate,
                   (*server)->tick().value());

    hog::server::SteadyTimer timer;
    if (const auto ran = (*server)->run(timer, stop_requested); !ran) {
        return EXIT_FAILURE;
    }
    hog::log::info(hog::app::server_log, "stopped at tick {}", (*server)->tick().value());
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span raw{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(raw.begin() + 1, raw.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-server: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
