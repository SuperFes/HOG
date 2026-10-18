#include <hog/bot/bot.hpp>
#include <hog/bot/options.hpp>
#include <hog/client/options.hpp>
#include <hog/core/log.hpp>
#include <hog/core/time.hpp>
#include <hog/login/service.hpp>
#include <hog/net/client.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/world.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

using namespace hog;
using namespace std::chrono_literals;

constexpr auto frame = std::chrono::milliseconds{33};
constexpr auto login_patience = std::chrono::seconds{10};

const log::Category bot_log{"bot"};

// A connect token from the login service, waited for; none if it won't give one in time.
Result<std::vector<std::byte>> log_in(const std::string& address, const std::string& name) {
    auto client = net::Client::connect(address);
    if (!client) {
        return std::unexpected(client.error());
    }
    std::vector<net::Event> events;
    const auto give_up = MonotonicClock::now() + login_patience;
    while (MonotonicClock::now() < give_up) {
        client->poll(events);
        for (const net::Event& event : events) {
            if (std::holds_alternative<net::Connected>(event)) {
                if (auto sent = client->send(
                        net::Delivery::Reliable,
                        login::encode(login::Ask{.version = login::login_version, .name = name}));
                    !sent) {
                    return std::unexpected(sent.error());
                }
            } else if (const auto* message = std::get_if<net::Message>(&event)) {
                const auto answer = login::decode(message->bytes());
                if (!answer) {
                    return std::unexpected(answer.error());
                }
                if (const auto* granted = std::get_if<login::Granted>(&*answer)) {
                    return granted->token;
                }
                if (const auto* denied = std::get_if<login::Denied>(&*answer)) {
                    return make_error(ErrorCode::InvalidArgument, denied->reason);
                }
            } else if (const auto* lost = std::get_if<net::Disconnected>(&event)) {
                return make_error(ErrorCode::Io, lost->reason);
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    return make_error(ErrorCode::Io, "the login service didn't answer in time");
}

int run(std::span<const std::string_view> arguments) {
    auto options = bot::parse_options(arguments);
    if (!options) {
        std::cerr << std::format("hog-bot: {}\n\n{}", options.error().message, bot::usage());
        return EXIT_FAILURE;
    }
    if (options->help) {
        std::cout << bot::usage();
        return EXIT_SUCCESS;
    }
    options->planet = client::locate(options->planet, std::filesystem::current_path());
    const auto planet = planet::load_planet_config(options->planet);
    if (!planet) {
        std::cerr << std::format("hog-bot: {}\n", planet.error());
        return EXIT_FAILURE;
    }
    const planet::World world{*planet};
    const planet::Archipelago& islands = world.geology().terrain().archipelago();

    std::vector<bot::Bot> bots;
    std::mt19937_64 scatter{options->seed};
    for (std::uint32_t which = 1; which <= options->count; ++which) {
        const std::string name = std::format("{}-{}", options->name, which);
        std::vector<std::byte> token;
        if (options->login) {
            auto granted = log_in(*options->login, name);
            if (!granted) {
                std::cerr << std::format("hog-bot: {} can't log in: {}\n", name, granted.error());
                return EXIT_FAILURE;
            }
            token = std::move(*granted);
        }
        // Scattered around the first island's middle, so they don't stand in one spot.
        const bot::Wander spot = bot::pick(islands.center(), world.geology().planet_radius(),
                                           scatter, {.furthest = 500.0, .nearest = 10.0});
        auto started = bot::Bot::start(world, options->connect, std::move(token), spot.target,
                                       options->seed + which);
        if (!started) {
            std::cerr << std::format("hog-bot: {} can't start: {}\n", name, started.error());
            return EXIT_FAILURE;
        }
        bots.push_back(std::move(*started));
        log::info(bot_log, "{} walking", name);
    }

    const MonotonicClock::time_point began = MonotonicClock::now();
    MonotonicClock::time_point last = began;
    while (true) {
        const MonotonicClock::time_point now = MonotonicClock::now();
        const double seconds = std::min(std::chrono::duration<double>(now - last).count(), 0.1);
        last = now;
        for (bot::Bot& bot : bots) {
            bot.update(now, seconds);
        }
        if (options->run_for && now - began >= *options->run_for) {
            return EXIT_SUCCESS;
        }
        std::this_thread::sleep_until(now + frame);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span<char*> given{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(given.begin() + 1, given.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-bot: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
