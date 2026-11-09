#include <hog/app/server.hpp>
#include <hog/bot/bot.hpp>
#include <hog/build_world/build_world.hpp>
#include <hog/client/session.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/net/server.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/store/sqlite_volume_archive.hpp>
#include <hog/testing/ports.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using hog::DVec3;
using hog::GameTime;
using hog::MonotonicClock;

namespace {

constexpr GameTime start{
    std::chrono::sys_days{std::chrono::year{2026} / 10 / 11}.time_since_epoch()};

// A server on a free players' port, for a small planet in `directory`.
std::unique_ptr<hog::app::Server> serving(const std::filesystem::path& directory) {
    std::ofstream{directory / "planet.toml"} << "[planet]\nseed = 1\nradius = 115000.0\n";
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

// Builds the ground about `direction` into the server's world file, as hog-build-world does.
void build_ground(const hog::globe::Generator& generator, const std::filesystem::path& directory,
                  DVec3 direction) {
    auto archive = hog::store::SqliteVolumeArchive::open(directory / "var" / "world.db");
    REQUIRE(archive);
    hog::jobs::JobSystem jobs{2};
    REQUIRE(hog::build_world::build(generator, *archive, jobs,
                                    {hog::globe::around(generator, direction, 3)}));
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
    DVec3 middle{0.0};
    {
        const auto unbuilt = serving(directory.path());
        const hog::globe::Generator& generator = unbuilt->generator();
        middle = hog::globe::land_near(generator, hog::planet::start_direction)
                     .value_or(hog::planet::start_direction);
        build_ground(generator, directory.path(), middle);
    }
    const auto server = serving(directory.path());
    auto bot = hog::bot::Bot::start(
        server->generator(), std::format("127.0.0.1:{}", server->players()->port()), {}, middle, 7);
    REQUIRE(bot);
    std::optional<hog::WorldPosition> began;

    std::atomic<bool> stop = false;
    const auto give_up = MonotonicClock::now() + 60s;
    MonotonicClock::time_point last = MonotonicClock::now();
    Visited timer{[&] {
        const MonotonicClock::time_point now = MonotonicClock::now();
        const double seconds = std::min(std::chrono::duration<double>(now - last).count(), 0.1);
        last = now;
        bot->update(now, seconds);
        // It stands once the ground under it is sent.
        if (!began && bot->standing()) {
            began = bot->feet();
        }
        // Far enough that only taken moves get there: the server isn't sending it back.
        if ((began && glm::length(bot->feet() - *began) >= 5.0) || now >= give_up) {
            stop = true;
        }
    }};
    REQUIRE(server->run(timer, [] { return start; }, stop));
    CHECK(bot->session().state() == hog::client::Session::State::Welcomed);
    REQUIRE(began);
    CHECK(glm::length(bot->feet() - began.value_or(bot->feet())) >= 5.0);
    // On the ground it was sent, which is the ground built, not falling through where none is.
    const DVec3 feet = bot->feet().metres();
    const double ground = hog::globe::top_of(bot->world(), glm::normalize(feet)).value_or(0.0);
    CHECK(ground == hog::globe::top_of(server->globe(), glm::normalize(feet)).value_or(-1.0));
    // Ground walked on is whole voxels, so within one of the surface its matter makes.
    CHECK(std::abs(glm::length(feet) - ground) < bot->world().layers().thickness);
    CHECK(server->players() != nullptr);
}
