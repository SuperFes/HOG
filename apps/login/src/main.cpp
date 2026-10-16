#include <hog/core/log.hpp>
#include <hog/core/time.hpp>
#include <hog/login/keys.hpp>
#include <hog/login/service.hpp>
#include <hog/login_app/options.hpp>
#include <hog/net/server.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <map>
#include <span>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

const hog::log::Category login_log{"login"};

// How long a client has, once connected, to ask.
constexpr auto ask_within = std::chrono::seconds{5};
constexpr auto poll_every = std::chrono::milliseconds{10};

// Set from a signal handler, which may only touch lock-free atomics, so it has to be global.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void request_stop(int /*signal*/) {
    stop_requested.store(true, std::memory_order_relaxed);
}

hog::login::UnixTime unix_now() {
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

// Answers a client's ask with a token or a denial, then closes its connection.
void answer(hog::net::Server& server, const hog::login::KeyPair& keys,
            const hog::net::Message& message) {
    const auto nonce = hog::login::random_seed();
    if (!nonce) {
        hog::log::error(login_log, "no nonce: {}", nonce.error());
        server.disconnect(message.from(), "the login service failed");
        return;
    }
    const hog::login::LoginMessage reply =
        hog::login::answer(message.bytes(), keys, unix_now(), *nonce);
    if (const auto* denied = std::get_if<hog::login::Denied>(&reply)) {
        hog::log::info(login_log, "denied: {}", denied->reason);
    } else {
        hog::log::info(login_log, "granted a token");
    }
    if (auto sent =
            server.send(message.from(), hog::net::Delivery::Reliable, hog::login::encode(reply));
        !sent) {
        hog::log::warn(login_log, "couldn't answer: {}", sent.error());
    }
    server.disconnect(message.from(), "answered");
}

// Answers each client's ask with a token or a denial, then closes it.
void serve(hog::net::Server& server, const hog::login::KeyPair& keys) {
    using hog::MonotonicClock;
    std::map<hog::net::ConnectionId, MonotonicClock::time_point> waiting;
    std::vector<hog::net::Event> events;
    while (!stop_requested.load(std::memory_order_relaxed)) {
        server.poll(events);
        for (const hog::net::Event& event : events) {
            if (const auto* connected = std::get_if<hog::net::Connected>(&event)) {
                waiting.emplace(connected->connection, MonotonicClock::now());
            } else if (const auto* message = std::get_if<hog::net::Message>(&event)) {
                if (waiting.erase(message->from()) == 0) {
                    continue;
                }
                answer(server, keys, *message);
            } else if (const auto* lost = std::get_if<hog::net::Disconnected>(&event)) {
                waiting.erase(lost->connection);
            }
        }
        const MonotonicClock::time_point now = MonotonicClock::now();
        std::erase_if(waiting, [&](const auto& entry) {
            if (now - entry.second < ask_within) {
                return false;
            }
            server.disconnect(entry.first, "nothing asked");
            return true;
        });
        std::this_thread::sleep_for(poll_every);
    }
}

int run(std::span<const std::string_view> arguments) {
    const auto options = hog::login_app::parse_options(arguments);
    if (!options) {
        std::cerr << std::format("hog-login: {}\n\n{}", options.error().message,
                                 hog::login_app::usage());
        return EXIT_FAILURE;
    }
    if (options->help) {
        std::cout << hog::login_app::usage();
        return EXIT_SUCCESS;
    }
    const auto keys =
        hog::login::load_or_make_keys(options->keys / "login.key", options->keys / "login.pub");
    if (!keys) {
        hog::log::error(login_log, "no signing key: {}", keys.error());
        return EXIT_FAILURE;
    }
    auto server = hog::net::Server::listen(options->port);
    if (!server) {
        hog::log::error(login_log, "cannot listen: {}", server.error());
        return EXIT_FAILURE;
    }
    for (const int signal : {SIGINT, SIGTERM}) {
        if (std::signal(signal, request_stop) == SIG_ERR) {
            hog::log::warn(login_log, "cannot handle signal {}", signal);
        }
    }
    hog::log::info(login_log, "handing out tokens on port {}; game servers check them with {}",
                   options->port, (options->keys / "login.pub").string());
    serve(*server, *keys);
    hog::log::info(login_log, "stopped");
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span raw{argv, static_cast<std::size_t>(argc)};
        const std::vector<std::string_view> arguments(raw.begin() + 1, raw.end());
        return run(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hog-login: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
