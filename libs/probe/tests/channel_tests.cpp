#include <hog/probe/channel.hpp>
#include <hog/probe/socket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace hog;
using namespace hog::probe;
using namespace std::chrono_literals;

namespace {

// Both ends of a connection over the loopback address.
struct Pair {
    Channel near;
    Channel far;
};

Pair connected() {
    auto listener = Listener::open(0);
    REQUIRE(listener);
    CHECK(listener->port() != 0);
    auto client = Socket::connect("127.0.0.1", listener->port());
    REQUIRE(client);
    auto pending = listener->pending(5s);
    REQUIRE(pending);
    REQUIRE(*pending);
    auto server = listener->accept();
    REQUIRE(server);
    return Pair{.near = Channel{std::move(*client)}, .far = Channel{std::move(*server)}};
}

Message received(Channel& channel) {
    auto message = channel.receive(5s);
    REQUIRE(message);
    REQUIRE(message->has_value());
    return message->value_or(Message{});
}

} // namespace

TEST_CASE("messages cross a connection both ways", "[probe][channel]") {
    Pair pair = connected();
    REQUIRE(pair.near.send(Hello{}));
    CHECK(received(pair.far) == Message{Hello{}});
    REQUIRE(pair.far.send(Refused{.reason = "busy"}));
    REQUIRE(pair.far.send(Refused{.reason = "still busy"}));
    CHECK(received(pair.near) == Message{Refused{.reason = "busy"}});
    CHECK(received(pair.near) == Message{Refused{.reason = "still busy"}});
}

TEST_CASE("a message larger than one read arrives whole", "[probe][channel]") {
    Pair pair = connected();
    Layers layers{.time = GameTime{std::chrono::days{3}}, .layers = {}};
    for (int index = 0; index < 8; ++index) {
        std::vector<float> values(100'000);
        for (std::size_t cell = 0; cell < values.size(); ++cell) {
            values[cell] = static_cast<float>(cell) * static_cast<float>(index);
        }
        layers.layers.push_back(Layer{.name = "layer " + std::to_string(index), .values = values});
    }
    // Over 3 MB, more than the socket buffers hold, so the sender needs the reader going.
    std::thread sender{[&pair, &layers] { CHECK(pair.near.send(layers)); }};
    const Message message = received(pair.far);
    sender.join();
    CHECK(message == Message{layers});
}

TEST_CASE("waiting for a message gives up after its timeout", "[probe][channel]") {
    Pair pair = connected();
    const auto started = std::chrono::steady_clock::now();
    auto message = pair.far.receive(50ms);
    REQUIRE(message);
    CHECK_FALSE(message->has_value());
    CHECK(std::chrono::steady_clock::now() - started >= 40ms);

    auto now = pair.far.receive(0ms);
    REQUIRE(now);
    CHECK_FALSE(now->has_value());
}

TEST_CASE("a closed peer fails the next receive", "[probe][channel]") {
    Pair pair = connected();
    REQUIRE(pair.near.send(Hello{}));
    pair.near.close();
    CHECK_FALSE(pair.near.is_open());
    // What was sent before closing still arrives.
    CHECK(received(pair.far) == Message{Hello{}});
    const auto after = pair.far.receive(5s);
    REQUIRE_FALSE(after);
    CHECK(after.error().code == ErrorCode::Io);
    CHECK_FALSE(pair.near.send(Hello{}));
}

TEST_CASE("garbage on the wire fails the receive", "[probe][channel]") {
    auto listener = Listener::open(0);
    REQUIRE(listener);
    auto client = Socket::connect("localhost", listener->port());
    REQUIRE(client);
    auto server = listener->accept();
    REQUIRE(server);
    const std::vector<std::byte> junk(16, std::byte{0xFF});
    REQUIRE(client->send(junk));
    Channel channel{std::move(*server)};
    const auto message = channel.receive(5s);
    REQUIRE_FALSE(message);
    CHECK(message.error().code == ErrorCode::Parse);
}

TEST_CASE("connecting where nothing listens fails", "[probe][channel]") {
    std::uint16_t port = 0;
    {
        auto listener = Listener::open(0);
        REQUIRE(listener);
        port = listener->port();
    }
    CHECK_FALSE(Socket::connect("127.0.0.1", port));
    CHECK_FALSE(Socket::connect("no.such.host.invalid", port));
}

TEST_CASE("a listener can't take a port in use", "[probe][channel]") {
    auto first = Listener::open(0);
    REQUIRE(first);
    CHECK_FALSE(Listener::open(first->port()));
}
