#include <hog/app/server.hpp>
#include <hog/client/session.hpp>
#include <hog/net/server.hpp>
#include <hog/net/simulation.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using hog::DVec3;
using hog::MonotonicClock;
using hog::WorldPosition;
using hog::client::Session;
namespace net = hog::net;
namespace protocol = hog::protocol;

namespace {

constexpr double radius = 115'000.0;
constexpr hog::GameTime start{
    std::chrono::sys_days{std::chrono::year{2026} / 10 / 11}.time_since_epoch()};

// On the surface, `metres` along it from the first player's place.
WorldPosition along(double metres) {
    return WorldPosition{DVec3{radius + 2.0, metres, 0.0}};
}

// A port from a random start, so tests running side by side rarely meet.
std::uint16_t random_port() {
    std::random_device random;
    return static_cast<std::uint16_t>(40'000 + (random() % 20'000));
}

net::Server listening() {
    const std::uint16_t first = random_port();
    for (std::uint16_t port = first; port < first + 100; ++port) {
        if (auto server = net::Server::listen(port)) {
            return std::move(*server);
        }
    }
    FAIL("no free port");
    std::unreachable();
}

Session connected(const net::Server& server) {
    auto session = Session::connect(std::format("127.0.0.1:{}", server.port()), {});
    REQUIRE(session);
    return std::move(*session);
}

// Simulates a bad network for as long as it lives.
class Simulated {
public:
    explicit Simulated(const net::Simulation& simulation) { net::simulate(simulation); }
    Simulated(const Simulated&) = delete;
    Simulated& operator=(const Simulated&) = delete;
    Simulated(Simulated&&) = delete;
    Simulated& operator=(Simulated&&) = delete;
    ~Simulated() { net::simulate({}); }
};

// A stand-in for the game server, recording what a session sends it.
struct Stand {
    net::Server server = listening();
    std::optional<net::ConnectionId> connection;
    std::vector<protocol::Message> heard;
    bool closed = false;
    std::vector<net::Event> events;

    void poll() {
        server.poll(events);
        for (const net::Event& event : events) {
            if (const auto* connected = std::get_if<net::Connected>(&event)) {
                connection = connected->connection;
            } else if (const auto* message = std::get_if<net::Message>(&event)) {
                auto decoded = protocol::decode(message->bytes());
                REQUIRE(decoded);
                heard.push_back(std::move(*decoded));
            } else if (std::holds_alternative<net::Disconnected>(event)) {
                closed = true;
            }
        }
    }

    void send(const protocol::Message& message) {
        REQUIRE(connection);
        REQUIRE(server.send(connection.value_or(net::ConnectionId{}), net::Delivery::Reliable,
                            protocol::encode(message)));
    }

    template <typename T>
    [[nodiscard]] std::size_t count() const {
        return static_cast<std::size_t>(
            std::ranges::count_if(heard, [](const protocol::Message& message) {
                return std::holds_alternative<T>(message);
            }));
    }
};

// Has the session look, from `eye`, until `done`, failing if that takes too long.
void look_until(Session& session, Stand& stand, const std::function<bool()>& done,
                WorldPosition eye = along(0.0)) {
    const auto give_up = MonotonicClock::now() + 10s;
    while (!done()) {
        if (MonotonicClock::now() >= give_up) {
            FAIL("gave up waiting");
        }
        session.look(eye, MonotonicClock::now());
        stand.poll();
        std::this_thread::sleep_for(1ms);
    }
}

// Welcomes a session to the stand.
void welcome(Session& session, Stand& stand) {
    look_until(session, stand, [&] { return stand.count<protocol::Hello>() == 1; });
    stand.send(protocol::Welcome{});
    look_until(session, stand, [&] { return session.state() == Session::State::Welcomed; });
}

} // namespace

TEST_CASE("a session says hello and is welcomed", "[client][session]") {
    Stand stand;
    auto session = Session::connect(std::format("127.0.0.1:{}", stand.server.port()),
                                    {std::byte{1}, std::byte{2}});
    REQUIRE(session);
    CHECK(session->state() == Session::State::Connecting);
    look_until(*session, stand, [&] { return stand.count<protocol::Hello>() == 1; });
    const auto& hello = std::get<protocol::Hello>(stand.heard.front());
    CHECK(hello.version == protocol::version);
    CHECK(hello.token == std::vector{std::byte{1}, std::byte{2}});
    // Nothing moves before the welcome.
    CHECK(stand.count<protocol::Moved>() == 0);

    stand.send(protocol::Welcome{});
    look_until(*session, stand, [&] { return session->state() == Session::State::Welcomed; });
    CHECK(session->replica().now().empty());
    CHECK_FALSE(session->last_snapshot());
}

TEST_CASE("a welcomed session says where the player is, as often as it may", "[client][session]") {
    Stand stand;
    Session session = connected(stand.server);
    welcome(session, stand);

    // Ahead of the real time the session has seen, so none of these is held back by it.
    const MonotonicClock::time_point later = MonotonicClock::now() + 1h;
    session.look(along(5.0), later);
    session.look(along(6.0), later + Session::move_interval / 2);
    session.look(along(7.0), later + Session::move_interval);
    const auto last_at = [&] {
        std::vector<protocol::Millimetres> at;
        for (const protocol::Message& message : stand.heard) {
            if (const auto* moved = std::get_if<protocol::Moved>(&message);
                moved != nullptr && moved->at != protocol::to_millimetres(along(0.0))) {
                at.push_back(moved->at);
            }
        }
        return at;
    };
    look_until(session, stand, [&] { return last_at().size() == 2; });
    CHECK(last_at() ==
          std::vector{protocol::to_millimetres(along(5.0)), protocol::to_millimetres(along(7.0))});
}

TEST_CASE("a session keeps what snapshots show and acknowledges them", "[client][session]") {
    Stand stand;
    Session session = connected(stand.server);
    welcome(session, stand);

    const protocol::Entered anna{
        .id = protocol::NetId{4}, .at = protocol::to_millimetres(along(100.0)), .name = "Anna"};
    stand.send(protocol::Snapshot{
        .tick = 1, .baseline = {}, .entered = {anna}, .changed = {}, .left = {}});
    look_until(session, stand, [&] { return stand.count<protocol::Ack>() == 1; });
    CHECK(std::ranges::find_if(stand.heard, [](const protocol::Message& message) {
              const auto* ack = std::get_if<protocol::Ack>(&message);
              return ack != nullptr && ack->tick == 1;
          }) != stand.heard.end());
    REQUIRE(session.replica().now().size() == 1);
    CHECK(session.replica().now()[0].name == "Anna");
    CHECK(session.last_snapshot());

    // One that doesn't fit is passed over, unacknowledged.
    stand.send(protocol::Snapshot{.tick = 2,
                                  .baseline = 1,
                                  .entered = {},
                                  .changed = {{.id = protocol::NetId{9}, .moved_by = {}}},
                                  .left = {}});
    stand.send(protocol::Snapshot{
        .tick = 3, .baseline = 1, .entered = {}, .changed = {}, .left = {anna.id}});
    look_until(session, stand, [&] { return stand.count<protocol::Ack>() == 2; });
    CHECK(session.replica().now().empty());
    CHECK(session.replica().tick() == 3);
}

TEST_CASE("a session says why it was refused or closed", "[client][session]") {
    SECTION("refused") {
        Stand stand;
        Session session = connected(stand.server);
        look_until(session, stand, [&] { return stand.count<protocol::Hello>() == 1; });
        stand.send(protocol::Refused{
            .reason = protocol::Refusal::Token, .version = protocol::version, .detail = {}});
        look_until(session, stand, [&] { return session.state() == Session::State::Refused; });
        CHECK_FALSE(session.reason().empty());
    }
    SECTION("closed") {
        Stand stand;
        Session session = connected(stand.server);
        welcome(session, stand);
        stand.server.disconnect(stand.connection.value_or(net::ConnectionId{}), "going away");
        look_until(session, stand, [&] { return session.state() == Session::State::Closed; });
        CHECK(session.reason().find("going away") != std::string::npos);
    }
}

TEST_CASE("a session is summed up for the network window", "[client][session]") {
    Stand stand;
    Session session = connected(stand.server);
    const std::string address = std::format("127.0.0.1:{}", stand.server.port());
    CHECK(hog::client::summarise(session, along(0.0), MonotonicClock::now()).connection ==
          address + ", connecting");
    welcome(session, stand);
    auto summary = hog::client::summarise(session, along(0.0), MonotonicClock::now());
    CHECK(summary.connection == address + ", welcomed");
    CHECK(summary.quality.starts_with("ping "));
    CHECK(summary.traffic.ends_with(" out"));
    CHECK(summary.snapshots == "no snapshots yet");
    CHECK(summary.nearby.empty());

    stand.send(protocol::Snapshot{.tick = 1,
                                  .baseline = {},
                                  .entered = {{.id = protocol::NetId{1},
                                               .at = protocol::to_millimetres(along(100.0)),
                                               .name = "Anna"},
                                              {.id = protocol::NetId{2},
                                               .at = protocol::to_millimetres(along(-30.0)),
                                               .name = "Kim"}},
                                  .changed = {},
                                  .left = {}});
    look_until(session, stand, [&] { return session.last_snapshot().has_value(); });
    const MonotonicClock::time_point later =
        session.last_snapshot().value_or(MonotonicClock::time_point{}) + 40ms;
    summary = hog::client::summarise(session, along(0.0), later);
    CHECK(summary.snapshots.ends_with(", newest 40 ms ago"));
    REQUIRE(summary.nearby.size() == 2);
    CHECK(summary.nearby[0].name == "Kim");
    CHECK(summary.nearby[0].distance == Catch::Approx(30.0));
    CHECK(summary.nearby[1].name == "Anna");

    stand.server.disconnect(stand.connection.value_or(net::ConnectionId{}), "going away");
    look_until(session, stand, [&] { return session.state() == Session::State::Closed; });
    CHECK(hog::client::summarise(session, along(0.0), later)
              .connection.starts_with(address + ", closed: "));
}

namespace {

// Real time, in which the players look while the server waits for it.
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

// A server on a free players' port, for a small planet in `directory`.
std::unique_ptr<hog::app::Server> serving(const std::filesystem::path& directory) {
    std::ofstream{directory / "planet.toml"} << "[planet]\nseed = 1\nradius_km = 115\n";
    hog::server::ServerConfig config;
    config.world = directory / "var" / "world.db";
    config.planet = directory / "planet.toml";
    config.metrics_interval = 0s;
    const std::uint16_t first = random_port();
    for (std::uint16_t port = first; port < first + 100; ++port) {
        config.net.port = port;
        auto server = hog::app::Server::open(config, start);
        REQUIRE(server);
        if ((*server)->players() != nullptr) {
            return std::move(*server);
        }
    }
    FAIL("no free port");
    std::unreachable();
}

// Whether `session` holds just one other, at `at`.
bool sees(const Session& session, WorldPosition at) {
    const auto& held = session.replica().now();
    return held.size() == 1 && held[0].at == protocol::to_millimetres(at);
}

} // namespace

TEST_CASE("two players see each other through a bad network", "[client][session]") {
    const hog::testing::TemporaryDirectory directory;
    const auto server = serving(directory.path());
    const std::string address = std::format("127.0.0.1:{}", server->players()->port());
    // In one process, both ends add it: about 150 ms and 6% loss each way.
    const Simulated simulated{net::Simulation{.lag = 75ms, .jitter = 0ms, .loss = 3, .reorder = 0}};
    auto anna = Session::connect(address, {});
    auto matt = Session::connect(address, {});
    REQUIRE(anna);
    REQUIRE(matt);

    WorldPosition matt_at = along(100.0);
    int stage = 0;
    std::atomic<bool> stop = false;
    const auto give_up = MonotonicClock::now() + 30s;
    Visited timer{[&] {
        const MonotonicClock::time_point now = MonotonicClock::now();
        anna->look(along(0.0), now);
        matt->look(matt_at, now);
        if (stage == 0 && sees(*anna, matt_at) && sees(*matt, along(0.0))) {
            stage = 1;
            matt_at = along(150.0);
        } else if (stage == 1 && sees(*anna, matt_at)) {
            stage = 2;
            stop = true;
        }
        if (now >= give_up) {
            stop = true;
        }
    }};
    REQUIRE(server->run(timer, [] { return start; }, stop));
    CHECK(stage == 2);
    CHECK(anna->state() == Session::State::Welcomed);
    CHECK(anna->replica().now().size() == 1);
    CHECK(anna->last_snapshot());
    CHECK(matt->stats());
}
