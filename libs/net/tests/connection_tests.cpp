#include <hog/net/client.hpp>
#include <hog/net/server.hpp>
#include <hog/net/simulation.hpp>
#include <hog/testing/ports.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace hog;
using namespace hog::net;
using namespace std::chrono_literals;

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::string text_of(const Message& message) {
    std::string text;
    for (const std::byte byte : message.bytes()) {
        text.push_back(static_cast<char>(byte));
    }
    return text;
}

// Everything either end has seen, gathered across polls.
struct Seen {
    std::vector<Connected> connected;
    std::vector<Message> messages;
    std::vector<Disconnected> disconnected;

    void add(std::vector<Event>& events) {
        for (Event& event : events) {
            if (const auto* connection = std::get_if<Connected>(&event)) {
                connected.push_back(*connection);
            } else if (auto* message = std::get_if<Message>(&event)) {
                messages.push_back(std::move(*message));
            } else {
                disconnected.push_back(std::get<Disconnected>(std::move(event)));
            }
        }
    }
};

// Both ends of a connection over loopback, polled together.
struct Pair {
    Server server;
    Client client;
    Seen at_server;
    Seen at_client;
    std::vector<Event> events;

    // Polls both ends until `done`, failing if that takes too long.
    void poll_until(const std::function<bool()>& done,
                    std::chrono::steady_clock::duration within = 10s) {
        const auto give_up = std::chrono::steady_clock::now() + within;
        while (!done()) {
            if (std::chrono::steady_clock::now() >= give_up) {
                FAIL("gave up waiting");
            }
            server.poll(events);
            at_server.add(events);
            client.poll(events);
            at_client.add(events);
            std::this_thread::sleep_for(1ms);
        }
    }

    [[nodiscard]] ConnectionId connection() const { return at_server.connected.front().connection; }
};

// A server on a free port.
Server listening() {
    for (int tries = 0; tries < hog::testing::port_tries; ++tries) {
        if (auto server = Server::listen(hog::testing::random_port())) {
            return std::move(*server);
        }
    }
    FAIL("no free port");
    std::unreachable();
}

// Simulates a bad network for as long as it lives.
class Simulated {
public:
    explicit Simulated(const Simulation& simulation) { simulate(simulation); }
    Simulated(const Simulated&) = delete;
    Simulated& operator=(const Simulated&) = delete;
    Simulated(Simulated&&) = delete;
    Simulated& operator=(Simulated&&) = delete;
    ~Simulated() { simulate({}); }
};

Pair connected_pair() {
    Server server = listening();
    auto client = Client::connect(std::format("127.0.0.1:{}", server.port()));
    REQUIRE(client);
    Pair pair{.server = std::move(server),
              .client = std::move(*client),
              .at_server = {},
              .at_client = {},
              .events = {}};
    pair.poll_until(
        [&] { return !pair.at_server.connected.empty() && !pair.at_client.connected.empty(); });
    return pair;
}

} // namespace

TEST_CASE("a client connects and both ends see it once") {
    const Pair pair = connected_pair();
    CHECK(pair.client.is_connected());
    CHECK(pair.at_server.connected.size() == 1);
    CHECK(pair.at_client.connected.size() == 1);
    CHECK(pair.at_server.disconnected.empty());
}

TEST_CASE("messages go each way, from their connection, with how they were sent") {
    Pair pair = connected_pair();
    REQUIRE(pair.client.send(Delivery::Reliable, bytes_of("hello")));
    pair.poll_until([&] { return !pair.at_server.messages.empty(); });
    const Message& hello = pair.at_server.messages.front();
    CHECK(text_of(hello) == "hello");
    CHECK(hello.delivery() == Delivery::Reliable);
    CHECK(hello.from() == pair.connection());

    REQUIRE(pair.server.send(pair.connection(), Delivery::Unreliable, bytes_of("now")));
    REQUIRE(pair.server.send(pair.connection(), Delivery::Bulk, bytes_of("later")));
    pair.poll_until([&] { return pair.at_client.messages.size() == 2; });
    for (const Message& message : pair.at_client.messages) {
        CHECK((text_of(message) == "now" ? Delivery::Unreliable : Delivery::Bulk) ==
              message.delivery());
    }
}

TEST_CASE("reliable messages arrive in the order they were sent") {
    Pair pair = connected_pair();
    constexpr int count = 200;
    for (int index = 0; index < count; ++index) {
        REQUIRE(pair.client.send(Delivery::Reliable, bytes_of(std::to_string(index))));
    }
    pair.poll_until([&] { return pair.at_server.messages.size() == count; });
    for (int index = 0; index < count; ++index) {
        CHECK(text_of(pair.at_server.messages[static_cast<std::size_t>(index)]) ==
              std::to_string(index));
    }
}

TEST_CASE("a reliable message overtakes bulk queued before it") {
    Pair pair = connected_pair();
    // Enough that it's still being sent when the reliable message is queued.
    constexpr int chunks = 4;
    const std::vector<std::byte> chunk(std::size_t{64} * 1024);
    for (int index = 0; index < chunks; ++index) {
        REQUIRE(pair.server.send(pair.connection(), Delivery::Bulk, chunk));
    }
    REQUIRE(pair.server.send(pair.connection(), Delivery::Reliable, bytes_of("urgent")));
    pair.poll_until([&] { return pair.at_client.messages.size() == chunks + 1; });
    const auto& messages = pair.at_client.messages;
    CHECK(messages.back().delivery() == Delivery::Bulk);
    for (std::size_t index = 0; index < messages.size(); ++index) {
        if (messages[index].delivery() == Delivery::Reliable) {
            CHECK(text_of(messages[index]) == "urgent");
            CHECK(index < messages.size() - 1);
        }
    }
}

TEST_CASE("closing a connection tells the other end why") {
    SECTION("from the client") {
        Pair pair = connected_pair();
        pair.client.disconnect("going home");
        CHECK_FALSE(pair.client.is_connected());
        pair.poll_until([&] { return !pair.at_server.disconnected.empty(); });
        CHECK(pair.at_server.disconnected.front().connection == pair.connection());
        CHECK(pair.at_server.disconnected.front().reason.contains("going home"));
        CHECK(pair.at_client.disconnected.empty());
    }
    SECTION("from the server, after a last message") {
        Pair pair = connected_pair();
        REQUIRE(pair.server.send(pair.connection(), Delivery::Reliable, bytes_of("goodbye")));
        pair.server.disconnect(pair.connection(), "server full");
        pair.poll_until([&] { return !pair.at_client.disconnected.empty(); });
        CHECK_FALSE(pair.client.is_connected());
        CHECK(pair.at_client.disconnected.front().reason.contains("server full"));
        REQUIRE(pair.at_client.messages.size() == 1);
        CHECK(text_of(pair.at_client.messages.front()) == "goodbye");
        CHECK(pair.at_server.disconnected.empty());
    }
}

TEST_CASE("sends that can't go are refused") {
    Pair pair = connected_pair();
    const std::vector<std::byte> too_large(max_message_size + 1);
    CHECK(pair.client.send(Delivery::Reliable, too_large).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(pair.server.send(ConnectionId{0}, Delivery::Reliable, bytes_of("anyone?")).error().code ==
          ErrorCode::NotFound);
    pair.client.disconnect("done");
    CHECK(pair.client.send(Delivery::Reliable, bytes_of("still there?")).error().code ==
          ErrorCode::NotFound);
}

TEST_CASE("a server needs a port") {
    CHECK(Server::listen(0).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("a client needs an IP address and port") {
    for (const std::string_view address : {"", "localhost", "127.0.0.1", "127.0.0.1:x"}) {
        CAPTURE(address);
        auto client = Client::connect(address);
        REQUIRE_FALSE(client);
        CHECK(client.error().code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("a connection's stats show its traffic, and only while it's there") {
    Pair pair = connected_pair();
    const std::vector<std::byte> chunk(std::size_t{16} * 1024);
    for (int index = 0; index < 8; ++index) {
        REQUIRE(pair.server.send(pair.connection(), Delivery::Reliable, chunk));
    }
    // The rates are averages the library updates every 5 seconds.
    pair.poll_until(
        [&] {
            const auto at_server = pair.server.stats(pair.connection());
            const auto at_client = pair.client.stats();
            return at_server && at_client && at_server->bytes_out_per_second > 0.0F &&
                   at_client->bytes_in_per_second > 0.0F;
        },
        20s);
    CHECK(pair.at_client.messages.size() == 8);
    // None fails as too slow.
    CHECK(pair.client.stats().value_or(Stats{.ping = 1h}).ping < 50ms);

    CHECK_FALSE(pair.server.stats(ConnectionId{0}));
    pair.client.disconnect("done");
    CHECK_FALSE(pair.client.stats());
}

TEST_CASE("simulated lag delays what passes, both ways") {
    constexpr auto lag = 25ms;
    const Simulated simulated{{.lag = lag}};
    Pair pair = connected_pair();
    const auto sent = std::chrono::steady_clock::now();
    REQUIRE(pair.client.send(Delivery::Reliable, bytes_of("ping")));
    pair.poll_until([&] { return !pair.at_server.messages.empty(); });
    REQUIRE(pair.server.send(pair.connection(), Delivery::Reliable, bytes_of("pong")));
    pair.poll_until([&] { return !pair.at_client.messages.empty(); });
    // Sent and received in this one process, each way: four times the lag.
    CHECK(std::chrono::steady_clock::now() - sent >= 4 * lag);
}

TEST_CASE("simulated loss drops unreliable messages") {
    Pair pair = connected_pair();
    constexpr int count = 200;
    {
        const Simulated simulated{{.loss = 50}};
        for (int index = 0; index < count; ++index) {
            REQUIRE(pair.server.send(pair.connection(), Delivery::Unreliable, bytes_of("maybe")));
            // Apart, so each goes in a packet of its own.
            std::this_thread::sleep_for(1ms);
        }
        // Long enough for every packet to have arrived or been dropped.
        const auto settled = std::chrono::steady_clock::now() + 100ms;
        pair.poll_until([&] { return std::chrono::steady_clock::now() >= settled; });
    }
    REQUIRE(pair.server.send(pair.connection(), Delivery::Reliable, bytes_of("surely")));
    pair.poll_until([&] {
        return !pair.at_client.messages.empty() &&
               text_of(pair.at_client.messages.back()) == "surely";
    });
    // Dropped half the time going out and half coming in, so about a quarter arrive.
    const auto arrived = static_cast<int>(pair.at_client.messages.size()) - 1;
    CHECK(arrived > count / 10);
    CHECK(arrived < count / 2);
}
