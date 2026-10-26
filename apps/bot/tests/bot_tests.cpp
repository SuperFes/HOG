#include <hog/app/server.hpp>
#include <hog/bot/bot.hpp>
#include <hog/client/session.hpp>
#include <hog/net/server.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/planet/world.hpp>
#include <hog/testing/ports.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
using hog::DVec3;
using hog::GameTime;
using hog::MonotonicClock;

namespace {

constexpr GameTime start{
    std::chrono::sys_days{std::chrono::year{2026} / 10 / 11}.time_since_epoch()};

// A server on a free players' port, for a small planet in `directory`.
std::unique_ptr<hog::app::Server> serving(const std::filesystem::path& directory) {
    std::ofstream{directory / "planet.toml"}
        << "[planet]\nseed = 1\nradius_km = 115\nerosion_cell = 1500\n";
    hog::server::ServerConfig config;
    config.world = directory / "var" / "world.db";
    config.planet = directory / "planet.toml";
    config.metrics_interval = 0s;
    for (int tries = 0; tries < hog::testing::port_tries; ++tries) {
        config.net.port = hog::testing::random_port();
        auto server = hog::app::Server::open(config, start);
        REQUIRE(server);
        if ((*server)->players() != nullptr) {
            return std::move(*server);
        }
    }
    FAIL("no free port");
    std::unreachable();
}

// Real time, visited between the server's sleeps.
class Visited {
public:
    explicit Visited(std::function<void()> visit) : visit_(std::move(visit)) {}

    [[nodiscard]] static MonotonicClock::time_point now() { return MonotonicClock::now(); }
    void sleep_until(MonotonicClock::time_point when) {
        do {
            visit_();
            std::this_thread::sleep_for(1ms);
        } while (now() < when);
    }

private:
    std::function<void()> visit_;
};

} // namespace

TEST_CASE("a bot joins a server and wanders the ground", "[bot]") {
    const hog::testing::TemporaryDirectory directory;
    const auto server = serving(directory.path());
    const hog::planet::World& world = server->planet();
    const DVec3 middle = world.geology().terrain().nearest_land(hog::planet::start_direction,
                                                                50 * hog::planet::fixed_one);

    auto bot = hog::bot::Bot::start(world, std::format("127.0.0.1:{}", server->players()->port()),
                                    {}, middle, 7);
    REQUIRE(bot);
    const hog::WorldPosition began = bot->feet();

    std::atomic<bool> stop = false;
    const auto give_up = MonotonicClock::now() + 60s;
    MonotonicClock::time_point last = MonotonicClock::now();
    Visited timer{[&] {
        const MonotonicClock::time_point now = MonotonicClock::now();
        const double seconds = std::min(std::chrono::duration<double>(now - last).count(), 0.1);
        last = now;
        bot->update(now, seconds);
        // Far enough that only taken moves get there: the server isn't sending it back.
        if (glm::length(bot->feet() - began) >= 5.0 || now >= give_up) {
            stop = true;
        }
    }};
    REQUIRE(server->run(timer, [] { return start; }, stop));
    CHECK(bot->session().state() == hog::client::Session::State::Welcomed);
    CHECK(glm::length(bot->feet() - began) >= 5.0);
    CHECK(server->players() != nullptr);
}
