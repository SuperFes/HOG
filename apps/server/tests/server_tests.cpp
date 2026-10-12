#include <hog/app/server.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;
using hog::GameTime;
using hog::MonotonicClock;
using hog::app::Server;

namespace {

constexpr GameTime start{
    std::chrono::sys_days{std::chrono::year{2026} / 10 / 11}.time_since_epoch()};

// Real time that moves only when the loop sleeps; asks the server to stop after some sleeps,
// which at one tick per sleep is that many ticks plus the one in progress.
class StoppingTimer {
public:
    StoppingTimer(std::atomic<bool>& stop, int sleeps) : stop_(&stop), sleeps_(sleeps) {}

    [[nodiscard]] MonotonicClock::time_point now() const { return time_; }
    void sleep_until(MonotonicClock::time_point when) {
        time_ = std::max(time_, when);
        if (--sleeps_ == 0) {
            stop_->store(true);
        }
    }

private:
    std::atomic<bool>* stop_;
    int sleeps_;
    MonotonicClock::time_point time_;
};

// A small planet and a world file in a fresh directory.
class World {
public:
    World() { write_planet(1); }

    void write_planet(std::uint64_t seed) const {
        std::ofstream{directory_.path() / "planet.toml"} << "[planet]\nseed = " << seed
                                                         << "\nradius_km = 115\n";
    }

    [[nodiscard]] std::filesystem::path write_ecology(std::string_view text) const {
        const auto path = directory_.path() / "ecology.toml";
        std::ofstream{path} << text;
        return path;
    }

    [[nodiscard]] hog::server::ServerConfig config() const {
        hog::server::ServerConfig config;
        config.world = directory_.path() / "var" / "world.db";
        config.planet = directory_.path() / "planet.toml";
        config.metrics_interval = 0s;
        return config;
    }

private:
    hog::testing::TemporaryDirectory directory_;
};

std::unique_ptr<Server> opened(const hog::server::ServerConfig& config, GameTime now = start) {
    auto server = Server::open(config, now);
    REQUIRE(server);
    return std::move(*server);
}

std::uint64_t counter(const Server& server, std::string_view name) {
    for (const auto& entry : server.metrics().entries()) {
        if (entry.name == name) {
            return std::get<std::unique_ptr<hog::metrics::Counter>>(entry.metric)->value();
        }
    }
    FAIL("no counter " << name);
    return 0;
}

// Runs the server for `ticks` live ticks.
void run(Server& server, int ticks) {
    std::atomic<bool> stop{false};
    StoppingTimer timer{stop, ticks - 1};
    REQUIRE(server.run(timer, stop));
}

} // namespace

TEST_CASE("a new world starts now and keeps its entities", "[app][server]") {
    const World world;
    {
        const auto server = opened(world.config());
        CHECK(server->tick() == hog::Tick{0});
        CHECK(server->time() == start);
        server->persistence().create().set<hog::server::Position>(
            {.at = hog::WorldPosition{1.0, 115'000.0, 2.0}});
        server->persistence().create();
        run(*server, 10);
        CHECK(server->tick() == hog::Tick{10});
        CHECK(server->time() == start + 500ms);
        CHECK(counter(*server, "hog_ticks_total") == 10);
        CHECK(counter(*server, "hog_snapshots_written_total") == 1);
    }

    const auto reopened = opened(world.config(), start + 1h);
    CHECK(reopened->tick() == hog::Tick{10});
    CHECK(reopened->time() == start + 500ms);
    CHECK(reopened->persistence().size() == 2);
    const auto first = reopened->persistence().find(1);
    REQUIRE(first);
    CHECK(first.value_or(flecs::entity{}).get<hog::server::Position>().at ==
          hog::WorldPosition{1.0, 115'000.0, 2.0});
}

TEST_CASE("a reopened world catches up to now", "[app][server]") {
    const World world;
    auto config = world.config();
    config.catch_up_ticks = 100;
    REQUIRE(opened(config)->save());

    const GameTime now = start + 24h;
    {
        const auto server = opened(config, now);
        const auto stats = server->catch_up([&] { return now; });
        CHECK(stats.ticks > 0);
        CHECK(stats.ticks <= 2 * config.catch_up_ticks);
        CHECK(server->time() <= now);
        CHECK(now - server->time() < 50ms);
        CHECK(counter(*server, "hog_catch_up_ticks_total") == stats.ticks);
        CHECK(now - server->weather().time() < config.weather.step);
        CHECK(counter(*server, "hog_weather_steps_total") > 0);

        // Already at now, there's nothing to do.
        CHECK(server->catch_up([&] { return now; }).ticks == 0);
    }
    // Catching up saved where it got to.
    CHECK(now - opened(config, now)->time() < 50ms);
}

TEST_CASE("startup time is caught up without saving", "[app][server]") {
    const World world;
    {
        const auto server = opened(world.config());
        CHECK(server->catch_up([] { return GameTime{start + 120ms}; }).ticks == 2);
        CHECK(server->tick() == hog::Tick{2});
    }
    CHECK(opened(world.config())->tick() == hog::Tick{0});
}

TEST_CASE("weather is kept with the world", "[app][server]") {
    const World world;
    auto config = world.config();
    config.catch_up_ticks = 100;
    REQUIRE(opened(config)->save());

    const GameTime later = start + 6h;
    hog::sim::WeatherFields before;
    int level = 0;
    {
        const auto server = opened(config, later);
        server->catch_up([&] { return later; });
        before = server->weather().fields();
        level = server->weather().grid().level();
        CHECK(server->weather().time() > start);
    }
    CHECK(opened(config, later)->weather().fields() == before);

    // Saved weather doesn't fit a coarser grid, so it starts over.
    config.weather.cell_size *= 4.0;
    const auto coarser = opened(config, later);
    CHECK(coarser->weather().grid().level() == level - 2);
    CHECK(coarser->weather().fields().size() == before.size() / 16);
}

TEST_CASE("wildlife lives with the weather and is kept with the world", "[app][server]") {
    const World world;
    auto config = world.config();
    config.catch_up_ticks = 100;
    CHECK(opened(config)->wildlife() == nullptr);

    config.wildlife.ecology = world.write_ecology(R"(
[forage]
capacity = { grassland = 100000, savanna = 100000, "temperate forest" = 50000 }

[[species]]
name = "deer"
diet = { forage = 1 }
habitat = { grassland = 1, savanna = 1, temperate_forest = 0.5 }
)");
    REQUIRE(opened(config)->save());
    const GameTime later = start + 72h;
    hog::sim::WildlifeFields before;
    {
        const auto server = opened(config, later);
        REQUIRE(server->wildlife() != nullptr);
        CHECK(server->wildlife()->total(0) > 0.0);
        server->catch_up([&] { return later; });
        CHECK(server->wildlife()->time() == later);
        CHECK(counter(*server, "hog_wildlife_steps_total") == 3);
        CHECK(server->wildlife()->grid().level() == server->weather().grid().level());
        before = server->wildlife()->fields();
    }
    CHECK(opened(config, later)->wildlife()->fields() == before);

    config.wildlife.ecology = world.write_ecology("[[species]]\nname = 3\n");
    const auto broken = Server::open(config, later);
    REQUIRE_FALSE(broken);
    CHECK(broken.error().message.find("ecology.toml") != std::string::npos);
}

TEST_CASE("snapshots are taken every save interval", "[app][server]") {
    const World world;
    auto config = world.config();
    config.save_interval = 100ms;
    const auto server = opened(config);
    run(*server, 10);
    // Five periodic snapshots over 500 ms of game time, then one on stopping. A newer snapshot
    // may replace one still waiting to be written.
    CHECK(counter(*server, "hog_snapshots_written_total") +
              counter(*server, "hog_snapshots_replaced_total") ==
          6);
    CHECK(counter(*server, "hog_snapshots_failed_total") == 0);
}

TEST_CASE("metrics are logged every interval", "[app][server]") {
    const World world;
    auto config = world.config();
    config.metrics_interval = 1s;
    const auto server = opened(config);
    hog::app::metrics_log.set_threshold(hog::log::Level::Info);

    std::vector<std::string> lines;
    class Capture final : public hog::log::Sink {
    public:
        explicit Capture(std::vector<std::string>& lines) : lines_(&lines) {}
        void write(const hog::log::Record& record) override {
            if (record.category == "metrics") {
                lines_->push_back(hog::log::format_line(record));
            }
        }

    private:
        std::vector<std::string>* lines_;
    };
    const auto sink = hog::log::add_sink(std::make_unique<Capture>(lines));
    run(*server, 50);
    hog::log::remove_sink(sink);

    // At 1 s and 2 s of real time, then on stopping.
    REQUIRE(lines.size() == 3);
    CHECK(lines.back().find(" hog_ticks_total=50 ") != std::string::npos);
    CHECK(lines.back().find(" hog_snapshots_written_total=1 ") != std::string::npos);
}

TEST_CASE("another world's snapshots are refused", "[app][server]") {
    const World world;
    REQUIRE(opened(world.config())->save());
    world.write_planet(2);
    const auto server = Server::open(world.config(), start);
    REQUIRE_FALSE(server);
    CHECK(server.error().code == hog::ErrorCode::InvalidArgument);
}

TEST_CASE("a server reports what it can't open", "[app][server]") {
    const World world;
    auto config = world.config();
    config.planet = config.planet.parent_path() / "missing.toml";
    const auto missing = Server::open(config, start);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == hog::ErrorCode::NotFound);

    config = world.config();
    config.world = config.planet;
    const auto not_a_database = Server::open(config, start);
    REQUIRE_FALSE(not_a_database);
    CHECK(not_a_database.error().message.starts_with(config.world.string() + ": "));
}
