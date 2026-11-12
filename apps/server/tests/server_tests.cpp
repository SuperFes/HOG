#include <hog/app/server.hpp>
#include <hog/build_world/build_world.hpp>
#include <hog/chat/filter.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/login/keys.hpp>
#include <hog/login/token.hpp>
#include <hog/net/client.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/store/sqlite_volume_archive.hpp>
#include <hog/testing/ports.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
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
                                                         << "\nradius = 115000.0\n";
    }

    [[nodiscard]] hog::server::ServerConfig config() const {
        hog::server::ServerConfig config;
        config.world = directory_.path() / "var" / "world.db";
        config.planet = directory_.path() / "planet.toml";
        config.metrics_interval = 0s;
        // The air is a world of its own to run; tests that want it turn it on.
        config.weather.on = false;
        return config;
    }

private:
    [[nodiscard]] std::filesystem::path write(std::string_view name, std::string_view text) const {
        const auto path = directory_.path() / name;
        std::ofstream{path} << text;
        return path;
    }

    hog::testing::TemporaryDirectory directory_;
};

std::unique_ptr<Server> opened(const hog::server::ServerConfig& config, GameTime now = start) {
    auto server = Server::open(config, now);
    REQUIRE(server);
    return std::move(*server);
}

// Writes the world's ground about `direction` into its file, as hog-build-world makes it.
void build_ground(const World& world, hog::DVec3 direction) {
    const auto server = opened(world.config());
    const hog::globe::Generator& generator = server->generator();
    auto archive = hog::store::SqliteVolumeArchive::open(world.config().world);
    REQUIRE(archive);
    hog::jobs::JobSystem jobs{2};
    REQUIRE(hog::build_world::build(generator, *archive, jobs,
                                    {hog::globe::around(generator, direction, 3)}));
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
    REQUIRE(server.run(timer, [] { return start; }, stop));
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

        // Already at now, there's nothing to do.
        CHECK(server->catch_up([&] { return now; }).ticks == 0);
    }
    // Catching up saved where it got to.
    CHECK(now - opened(config, now)->time() < 50ms);
}

TEST_CASE("a world with weather makes it as the clock runs", "[app][server]") {
    const World world;
    auto config = world.config();
    config.weather = {.on = true, .layers = 8, .spin_up = 0h};
    const auto server = opened(config);
    REQUIRE(server->weather());
    // Four game minutes outrun the keeper's interval of two; its job is taken in on a later tick.
    GameTime now = start + 4min;
    for (int tries = 0; tries < 10'000 && !server->weather()->latest(); ++tries) {
        server->catch_up([&] { return now; });
        now += 100ms;
        std::this_thread::sleep_for(1ms);
    }
    CHECK(server->weather()->latest());
}

TEST_CASE("a reopened world has the air it saved, without spinning it up again", "[app][server]") {
    const World world;
    auto config = world.config();
    config.weather = {.on = true, .layers = 8, .spin_up = 0h};
    {
        const auto server = opened(config);
        GameTime now = start + 4min;
        for (int tries = 0; tries < 10'000 && !server->weather()->latest(); ++tries) {
            server->catch_up([&] { return now; });
            now += 100ms;
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(server->weather()->latest());
        REQUIRE(server->save());
    }
    // A long spin-up would keep a new world's weather from showing for good; a kept one shows.
    config.weather.spin_up = 48h;
    const auto reopened = opened(config, start);
    REQUIRE(reopened->weather());
    CHECK(reopened->weather()->latest());
}

TEST_CASE("a world without weather has none", "[app][server]") {
    const World world;
    CHECK(opened(world.config())->weather() == nullptr);
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

TEST_CASE("a new world can start at a chosen moment", "[app][server]") {
    const World world;
    auto config = world.config();
    config.debug.start = start + std::chrono::days{365};
    CHECK(opened(config)->time() == start + std::chrono::days{365});
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

namespace {} // namespace

// Phase 2's gate, run by hand on a release build: hog_app_tests "[.gate]". It also prints the
// world month by month, to judge whether seasons and populations look plausible.
namespace {

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

// Open on a free players' port.
std::unique_ptr<Server> playing(hog::server::ServerConfig config) {
    for (int tries = 0; tries < hog::testing::port_tries; ++tries) {
        config.net.port = hog::testing::random_port();
        auto server = opened(config);
        if (server->players() != nullptr) {
            return server;
        }
    }
    FAIL("no free port");
    std::unreachable();
}

std::unique_ptr<Server> playing(const World& world, bool time_control = false) {
    auto config = world.config();
    config.debug.time_control = time_control;
    return playing(config);
}

} // namespace

namespace {

// A player logged in as `account`, who goes on saying they're at `place` once welcomed.
class Returning {
public:
    Returning(const Server& server, const hog::login::KeyPair& keys, std::uint64_t account,
              hog::protocol::Placed place)
        : place_(place), client_(connected(server)) {
        const auto issued = std::chrono::floor<std::chrono::seconds>(hog::WallClock::now());
        token_ = hog::login::sign({.account = account,
                                   .name = "Matt",
                                   .issued = issued,
                                   .expires = issued + hog::login::token_life,
                                   .nonce = {}},
                                  keys);
    }

    void visit() {
        client_.poll(events_);
        for (const hog::net::Event& event : events_) {
            if (std::holds_alternative<hog::net::Connected>(event)) {
                REQUIRE(client_.send(hog::net::Delivery::Reliable,
                                     hog::protocol::encode(hog::protocol::Hello{
                                         .version = hog::protocol::version, .token = token_})));
            } else if (const auto* message = std::get_if<hog::net::Message>(&event)) {
                auto decoded = hog::protocol::decode(message->bytes());
                REQUIRE(decoded);
                if (std::holds_alternative<hog::protocol::Welcome>(*decoded)) {
                    welcomed_ = MonotonicClock::now();
                } else if (const auto* placed = std::get_if<hog::protocol::Placed>(&*decoded)) {
                    placed_ = *placed;
                }
            }
        }
        if (welcomed_) {
            REQUIRE(
                client_.send(hog::net::Delivery::Unreliable,
                             hog::protocol::encode(hog::protocol::Moved{.sequence = ++moves_,
                                                                        .corrections = 0,
                                                                        .at = place_.at,
                                                                        .stance = place_.stance,
                                                                        .facing = place_.facing})));
        }
    }

    // Long enough after the welcome for the moves sent to have been taken.
    [[nodiscard]] bool settled() const {
        return welcomed_.has_value() && MonotonicClock::now() - welcomed_.value() >= 200ms;
    }
    [[nodiscard]] const std::optional<hog::protocol::Placed>& placed() const { return placed_; }

private:
    static hog::net::Client connected(const Server& server) {
        auto client =
            hog::net::Client::connect(std::format("127.0.0.1:{}", server.players()->port()));
        REQUIRE(client);
        return std::move(*client);
    }

    hog::protocol::Placed place_;
    hog::net::Client client_;
    std::vector<std::byte> token_;
    std::vector<hog::net::Event> events_;
    std::optional<MonotonicClock::time_point> welcomed_;
    std::optional<hog::protocol::Placed> placed_;
    std::uint32_t moves_ = 0;
};

} // namespace

TEST_CASE("a logged in player is placed where they were when the server stopped", "[app][server]") {
    const World world;
    const hog::testing::TemporaryDirectory keys_directory;
    const auto keys = hog::login::load_or_make_keys(keys_directory.path() / "login.seed",
                                                    keys_directory.path() / "login.pub");
    REQUIRE(keys);
    auto config = world.config();
    config.net.login_key = keys_directory.path() / "login.pub";
    const double radius = 115'000.0;
    const hog::protocol::Placed flying{.at = hog::protocol::to_millimetres(hog::WorldPosition{
                                           hog::DVec3{0.0, radius + 300.0, 0.0}}),
                                       .stance = hog::protocol::Stance::Flying,
                                       .facing = {.heading = 12'345, .pitch = -678}};
    {
        const auto server = playing(config);
        Returning matt{*server, *keys, 7, flying};
        std::atomic<bool> stop = false;
        const auto give_up = MonotonicClock::now() + 10s;
        Visited timer{[&] {
            matt.visit();
            if (matt.settled() || MonotonicClock::now() >= give_up) {
                stop = true;
            }
        }};
        REQUIRE(server->run(timer, [] { return start; }, stop));
        REQUIRE(matt.settled());
        // Never here before.
        CHECK_FALSE(matt.placed());
    }
    const auto server = playing(config);
    Returning matt{*server, *keys, 7, flying};
    Returning anna{*server, *keys, 8, flying};
    std::atomic<bool> stop = false;
    const auto give_up = MonotonicClock::now() + 10s;
    Visited timer{[&] {
        matt.visit();
        anna.visit();
        if ((matt.placed() && anna.settled()) || MonotonicClock::now() >= give_up) {
            stop = true;
        }
    }};
    REQUIRE(server->run(timer, [] { return start; }, stop));
    CHECK(matt.placed() == flying);
    CHECK_FALSE(anna.placed());
}

TEST_CASE("players hear each other's chat, named by the server", "[app][server]") {
    const World world;
    const auto server = playing(world);
    const std::string address = std::format("127.0.0.1:{}", server->players()->port());
    struct Talker {
        hog::net::Client client;
        bool welcomed = false;
        std::optional<hog::protocol::Said> chat;
        std::vector<hog::net::Event> events;
    };
    auto first = hog::net::Client::connect(address);
    auto second = hog::net::Client::connect(address);
    REQUIRE(first);
    REQUIRE(second);
    std::array<Talker, 2> talkers{
        Talker{.client = std::move(*first), .welcomed = false, .chat = {}, .events = {}},
        Talker{.client = std::move(*second), .welcomed = false, .chat = {}, .events = {}}};
    bool said = false;
    std::atomic<bool> stop = false;
    const auto give_up = MonotonicClock::now() + 10s;
    Visited timer{[&] {
        for (Talker& talker : talkers) {
            talker.client.poll(talker.events);
            for (const hog::net::Event& event : talker.events) {
                if (std::holds_alternative<hog::net::Connected>(event)) {
                    REQUIRE(talker.client.send(hog::net::Delivery::Reliable,
                                               hog::protocol::encode(hog::protocol::Hello{})));
                } else if (const auto* message = std::get_if<hog::net::Message>(&event)) {
                    auto decoded = hog::protocol::decode(message->bytes());
                    REQUIRE(decoded);
                    if (std::holds_alternative<hog::protocol::Welcome>(*decoded)) {
                        talker.welcomed = true;
                    } else if (auto* line = std::get_if<hog::protocol::Said>(&*decoded)) {
                        talker.chat = std::move(*line);
                    }
                }
            }
        }
        if (!said && talkers[0].welcomed && talkers[1].welcomed) {
            said = true;
            REQUIRE(talkers[0].client.send(
                hog::net::Delivery::Reliable,
                hog::protocol::encode(hog::protocol::Said{.from = "liar", .text = "hello all"})));
        }
        if ((talkers[0].chat && talkers[1].chat) || MonotonicClock::now() >= give_up) {
            stop = true;
        }
    }};
    REQUIRE(server->run(timer, [] { return start; }, stop));
    REQUIRE(talkers[0].chat);
    REQUIRE(talkers[1].chat);
    const hog::protocol::Said to_speaker = talkers[0].chat.value_or(hog::protocol::Said{});
    const hog::protocol::Said to_hearer = talkers[1].chat.value_or(hog::protocol::Said{});
    CHECK(to_speaker.text == "hello all");
    CHECK(to_hearer.text == "hello all");
    // The server names the speaker itself, never taking the name the client claims.
    CHECK(to_speaker.from != "liar");
    CHECK_FALSE(to_speaker.from.empty());
    CHECK(to_hearer.from == to_speaker.from);
    CHECK(counter(*server, "hog_chat_lines_total") == 2);
}

TEST_CASE("hate speech is reported and relayed as small talk", "[app][server]") {
    const World world;
    const auto server = playing(world);
    auto connected =
        hog::net::Client::connect(std::format("127.0.0.1:{}", server->players()->port()));
    REQUIRE(connected);
    hog::net::Client& client = *connected;
    bool welcomed = false;
    std::optional<hog::protocol::Said> echoed;
    std::vector<hog::net::Event> events;
    std::atomic<bool> stop = false;
    const auto give_up = MonotonicClock::now() + 10s;
    Visited timer{[&] {
        client.poll(events);
        for (const hog::net::Event& event : events) {
            if (std::holds_alternative<hog::net::Connected>(event)) {
                REQUIRE(client.send(hog::net::Delivery::Reliable,
                                    hog::protocol::encode(hog::protocol::Hello{})));
            } else if (const auto* message = std::get_if<hog::net::Message>(&event)) {
                auto decoded = hog::protocol::decode(message->bytes());
                REQUIRE(decoded);
                if (std::holds_alternative<hog::protocol::Welcome>(*decoded)) {
                    welcomed = true;
                    REQUIRE(client.send(hog::net::Delivery::Reliable,
                                        hog::protocol::encode(hog::protocol::Said{
                                            .from = "", .text = "you n1ggers get out"})));
                } else if (auto* line = std::get_if<hog::protocol::Said>(&*decoded)) {
                    echoed = std::move(*line);
                }
            }
        }
        if (echoed || MonotonicClock::now() >= give_up) {
            stop = true;
        }
    }};
    REQUIRE(server->run(timer, [] { return start; }, stop));
    REQUIRE(welcomed);
    REQUIRE(echoed);
    const hog::protocol::Said heard = echoed.value_or(hog::protocol::Said{});
    // Even the speaker reads small talk; nobody is sent the slur.
    CHECK(heard.text != "you n1ggers get out");
    CHECK_FALSE(hog::chat::hateful(heard.text));
    CHECK(counter(*server, "hog_chat_hateful_lines_total") == 1);
    CHECK(counter(*server, "hog_chat_lines_total") == 1);
}

TEST_CASE("a player's wish runs the world's clock faster, where allowed", "[app][server]") {
    const World world;
    const auto server = playing(world, true);
    auto connected =
        hog::net::Client::connect(std::format("127.0.0.1:{}", server->players()->port()));
    REQUIRE(connected);
    hog::net::Client& client = *connected;
    std::optional<hog::protocol::Clock> clock;
    std::vector<hog::net::Event> events;
    std::atomic<bool> stop = false;
    const auto give_up = MonotonicClock::now() + 10s;
    Visited timer{[&] {
        client.poll(events);
        for (const hog::net::Event& event : events) {
            if (std::holds_alternative<hog::net::Connected>(event)) {
                REQUIRE(client.send(hog::net::Delivery::Reliable,
                                    hog::protocol::encode(hog::protocol::Hello{})));
            } else if (const auto* message = std::get_if<hog::net::Message>(&event)) {
                auto decoded = hog::protocol::decode(message->bytes());
                REQUIRE(decoded);
                if (std::holds_alternative<hog::protocol::Welcome>(*decoded)) {
                    REQUIRE(client.send(hog::net::Delivery::Reliable,
                                        hog::protocol::encode(hog::protocol::Hurry{.scale = 512})));
                } else if (const auto* told = std::get_if<hog::protocol::Clock>(&*decoded)) {
                    clock = *told;
                }
            }
        }
        // Once told, hold on until the sped-up world shows in its time.
        if ((clock && server->time() - start >= 1min) || MonotonicClock::now() >= give_up) {
            stop = true;
        }
    }};
    REQUIRE(server->run(timer, [] { return start; }, stop));
    REQUIRE(clock);
    CHECK(clock.value_or(hog::protocol::Clock{}).scale == 512);
    CHECK(server->time() - start >= 1min);
}

TEST_CASE("a world without time control stays at real time", "[app][server]") {
    const World world;
    const auto server = playing(world);
    auto connected =
        hog::net::Client::connect(std::format("127.0.0.1:{}", server->players()->port()));
    REQUIRE(connected);
    hog::net::Client& client = *connected;
    bool clock_told = false;
    std::optional<MonotonicClock::time_point> echoed;
    std::vector<hog::net::Event> events;
    std::atomic<bool> stop = false;
    const auto give_up = MonotonicClock::now() + 10s;
    Visited timer{[&] {
        client.poll(events);
        for (const hog::net::Event& event : events) {
            if (std::holds_alternative<hog::net::Connected>(event)) {
                REQUIRE(client.send(hog::net::Delivery::Reliable,
                                    hog::protocol::encode(hog::protocol::Hello{})));
            } else if (const auto* message = std::get_if<hog::net::Message>(&event)) {
                auto decoded = hog::protocol::decode(message->bytes());
                REQUIRE(decoded);
                if (std::holds_alternative<hog::protocol::Welcome>(*decoded)) {
                    REQUIRE(client.send(hog::net::Delivery::Reliable,
                                        hog::protocol::encode(hog::protocol::Hurry{.scale = 512})));
                    // Chat rides behind the wish, so its echo brackets the server handling it.
                    REQUIRE(client.send(hog::net::Delivery::Reliable,
                                        hog::protocol::encode(hog::protocol::Said{
                                            .from = "", .text = "still with us?"})));
                } else if (std::holds_alternative<hog::protocol::Said>(*decoded)) {
                    echoed = echoed.value_or(MonotonicClock::now());
                } else if (std::holds_alternative<hog::protocol::Clock>(*decoded)) {
                    clock_told = true;
                }
            }
        }
        const auto now = MonotonicClock::now();
        if ((echoed && now - *echoed >= 300ms) || now >= give_up) {
            stop = true;
        }
    }};
    REQUIRE(server->run(timer, [] { return start; }, stop));
    REQUIRE(echoed);
    CHECK_FALSE(clock_told);
    CHECK(server->time() - start < 1min);
}

TEST_CASE("a hole dug in the voxel world is kept with it", "[app][server]") {
    const World world;
    const hog::DVec3 direction = hog::planet::start_direction;
    build_ground(world, direction);
    hog::globe::Site site;
    {
        auto server = opened(world.config());
        const hog::globe::Generator& generator = server->generator();
        const hog::globe::Layers& layers = generator.layers();
        const double ground =
            hog::globe::ground_below(generator, direction, hog::globe::voxel_lod(generator))
                .value_or(generator.planet().body.radius);
        const std::vector<hog::DVec3> here{direction * ground};
        server->hold(here);
        const hog::globe::Cell cell = hog::globe::cell_under(direction, layers.level);
        site = {.face = cell.face,
                .i = cell.i,
                .j = cell.j,
                .layer = static_cast<std::uint32_t>(layers.layer_at(ground - 1.0))};
        hog::globe::World globe = server->globe();
        CHECK(globe.at(site).solid != hog::globe::Material::Air);
        hog::globe::dig(globe, direction * ground, 3.0);
        CHECK_FALSE(server->volume().unsaved().empty());
        CHECK(globe.at(site).solid == hog::globe::Material::Air);
        REQUIRE(server->save());
    }
    auto reopened = opened(world.config());
    CHECK(reopened->volume().columns().empty());
    const std::vector<hog::DVec3> here{direction * reopened->generator().planet().body.radius};
    reopened->hold(here);
    CHECK_FALSE(reopened->volume().columns().empty());
    CHECK(reopened->volume().unsaved().empty());
    CHECK(reopened->globe().at(site).solid == hog::globe::Material::Air);
}

namespace {

// A client digging, and what it's been sent.
struct Digger {
    hog::net::Client client;
    bool welcomed = false;
    // Each finest tile sent, how many times, and as last sent.
    std::map<hog::globe::TileKey, std::pair<int, std::vector<std::byte>>> tiles;
    std::vector<hog::net::Event> events;
};

// Takes in what the server sent a digger, saying hello once connected.
void hear(Digger& digger) {
    digger.client.poll(digger.events);
    for (const hog::net::Event& event : digger.events) {
        if (std::holds_alternative<hog::net::Connected>(event)) {
            REQUIRE(digger.client.send(hog::net::Delivery::Reliable,
                                       hog::protocol::encode(hog::protocol::Hello{})));
        } else if (const auto* message = std::get_if<hog::net::Message>(&event)) {
            auto decoded = hog::protocol::decode(message->bytes());
            REQUIRE(decoded);
            if (std::holds_alternative<hog::protocol::Welcome>(*decoded)) {
                digger.welcomed = true;
            } else if (auto* sent = std::get_if<hog::protocol::Tiles>(&*decoded)) {
                for (hog::protocol::SentTile& tile : sent->tiles) {
                    if (tile.level == 0) {
                        auto& [times, bytes] = digger.tiles[{
                            .level = 0, .face = tile.face, .i = tile.i, .j = tile.j, .k = tile.k}];
                        ++times;
                        bytes = std::move(tile.bytes);
                    }
                }
            }
        }
    }
}

} // namespace

// Hidden while digging changes the volume and players are sent the land.
TEST_CASE("a player's digging has the tiles it touches sent again to everyone",
          "[.][app][server]") {
    const World world;
    build_ground(world, hog::planet::start_direction);
    const auto server = playing(world);
    const std::string address = std::format("127.0.0.1:{}", server->players()->port());
    const hog::globe::Generator& generator = server->generator();
    const hog::DVec3 direction = hog::planet::start_direction;
    const double ground =
        hog::globe::ground_below(generator, direction, hog::globe::voxel_lod(generator))
            .value_or(generator.planet().body.radius);
    auto first = hog::net::Client::connect(address);
    auto second = hog::net::Client::connect(address);
    REQUIRE(first);
    REQUIRE(second);
    std::array<Digger, 2> diggers{
        Digger{.client = std::move(*first), .welcomed = false, .tiles = {}, .events = {}},
        Digger{.client = std::move(*second), .welcomed = false, .tiles = {}, .events = {}}};
    const hog::globe::Cell cell = hog::globe::cell_under(direction, generator.layers().level);
    constexpr auto side = static_cast<std::uint32_t>(hog::globe::Volume::tile_side);
    const auto layer = static_cast<std::uint32_t>(generator.layers().layer_at(ground - 1.0));
    const hog::globe::TileKey under{
        .level = 0, .face = cell.face, .i = cell.i / side, .j = cell.j / side, .k = layer / side};
    const auto sent_again = [&](const Digger& digger) {
        const auto found = digger.tiles.find(under);
        return found != digger.tiles.end() && found->second.first > 1;
    };
    bool placed = false;
    bool dug = false;
    std::atomic<bool> stop = false;
    const auto give_up = MonotonicClock::now() + 60s;
    Visited timer{[&] {
        for (Digger& digger : diggers) {
            hear(digger);
        }
        // Players are shown the surfaces where they are, so each says where.
        if (!placed && diggers[0].welcomed && diggers[1].welcomed) {
            placed = true;
            for (Digger& digger : diggers) {
                REQUIRE(digger.client.send(hog::net::Delivery::Reliable,
                                           hog::protocol::encode(hog::protocol::Moved{
                                               .sequence = 1,
                                               .at = hog::protocol::to_millimetres(
                                                   hog::WorldPosition{direction * (ground + 2.0)}),
                                               .stance = hog::protocol::Stance::Flying})));
            }
        }
        // Dug once each holds the tile it's over.
        const auto holds = [&](const Digger& digger) { return digger.tiles.contains(under); };
        if (placed && !dug && holds(diggers[0]) && holds(diggers[1])) {
            dug = true;
            REQUIRE(diggers[0].client.send(
                hog::net::Delivery::Reliable,
                hog::protocol::encode(hog::protocol::Dug{
                    .centre = hog::protocol::to_millimetres(hog::WorldPosition{direction * ground}),
                    .radius = 3'000})));
        }
        if ((dug && sent_again(diggers[0]) && sent_again(diggers[1])) ||
            MonotonicClock::now() >= give_up) {
            stop = true;
        }
    }};
    REQUIRE(server->run(timer, [] { return start; }, stop));
    REQUIRE(server->globe().revision() > 0);
    // Both are sent the tile under the hole again once it was dug, as the server holds it.
    for (const Digger& digger : diggers) {
        INFO("tiles held " << digger.tiles.size() << ", dug " << dug);
        CHECK(sent_again(digger));
        const auto found = digger.tiles.find(under);
        REQUIRE(found != digger.tiles.end());
        // The hole is in it: the flow carries on changing the ground after, so it may be sent
        // again later still.
        const auto held = hog::globe::VoxelTree::decode(found->second.second);
        REQUIRE(held);
        CHECK(held->at(static_cast<int>(cell.i % side), static_cast<int>(cell.j % side),
                       static_cast<int>(layer % side))
                  .solid == hog::globe::Material::Air);
    }
}
