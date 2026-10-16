#include <hog/protocol/messages.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <variant>
#include <vector>

using hog::ErrorCode;
using namespace hog::protocol;

namespace {

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

// Somewhere on an Earth-sized planet's surface.
constexpr Millimetres far_out{3'000'000'000, -4'500'000'123, 2'000'000'000};

Snapshot whole() {
    return {.tick = 7,
            .baseline = {},
            .entered = {{.id = NetId{1}, .at = far_out, .name = "Matt"},
                        {.id = NetId{900}, .at = -far_out, .name = ""}},
            .changed = {},
            .left = {}};
}

Snapshot delta() {
    return {.tick = 120,
            .baseline = 117,
            .entered = {{.id = NetId{4}, .at = far_out, .name = std::string(most_name, 'n')}},
            .changed = {{.id = NetId{1}, .moved_by = Millimetres{300, -120, 0}},
                        {.id = NetId{2}, .moved_by = {}},
                        {.id = NetId{70'000}, .moved_by = Millimetres{0, 0, -5}}},
            .left = {NetId{3}, NetId{0xFFFF'FFFF}}};
}

std::vector<Message> examples() {
    return {Hello{},
            Hello{.version = version, .token = bytes({1, 2, 3, 250})},
            Welcome{},
            Refused{.reason = Refusal::Version, .version = 7, .detail = "too new"},
            Refused{.reason = Refusal::Silent, .version = version, .detail = ""},
            Moved{.sequence = 0xFFFF'FFFF, .corrections = 3, .at = far_out},
            Corrected{.count = 1, .at = far_out},
            Ack{.tick = 0xFFFF'FFF0},
            whole(),
            delta(),
            Snapshot{.tick = 1, .baseline = 0, .entered = {}, .changed = {}, .left = {}}};
}

// A snapshot at tick 10 with no baseline and nothing entering, then `rest`.
std::vector<std::byte> snapshot_with(std::initializer_list<int> rest) {
    auto out = bytes({6, 0, 10, 0, 0, 0, 0, 0});
    for (const int value : rest) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

} // namespace

TEST_CASE("protocol messages read back as written", "[protocol]") {
    for (const Message& message : examples()) {
        const auto read = decode(encode(message));
        REQUIRE(read);
        CHECK(*read == message);
    }
}

TEST_CASE("a hello starts with its kind and version, as in every version", "[protocol]") {
    const auto written = encode(Hello{.version = version, .token = {}});
    REQUIRE(written.size() >= 6);
    CHECK(written[0] == std::byte{1});
    CHECK(written[1] == std::byte{0});
    CHECK(written[2] == static_cast<std::byte>(version & 0xFFU));
}

TEST_CASE("a hello of another version is read for its version alone", "[protocol]") {
    // Whatever a later version puts after the version, it isn't read.
    const auto read = decode(bytes({1, 0, 99, 0, 0, 0, 7, 7, 7}));
    REQUIRE(read);
    const auto* hello = std::get_if<Hello>(&*read);
    REQUIRE(hello != nullptr);
    CHECK(hello->version == 99);
    CHECK(hello->token.empty());
}

TEST_CASE("messages cut short or with bytes left over are refused", "[protocol]") {
    for (const Message& message : examples()) {
        const auto whole = encode(message);
        for (std::size_t length = 0; length < whole.size(); ++length) {
            CAPTURE(length);
            const auto read = decode(std::span{whole}.first(length));
            REQUIRE_FALSE(read);
            CHECK(read.error().code == ErrorCode::Parse);
        }
        auto longer = whole;
        longer.push_back(std::byte{0});
        CHECK_FALSE(decode(longer));
    }
}

TEST_CASE("unknown kinds and refusals, and tokens too long, are refused", "[protocol]") {
    CHECK(decode(bytes({9, 0})).error().code == ErrorCode::Unsupported);
    CHECK(decode(bytes({3, 0, 1, 0, 0, 0, 200, 0, 0})).error().code == ErrorCode::Parse);
    // A hello of this version with no token, but saying it has one too long.
    std::vector<std::byte> long_token = encode(Hello{});
    long_token.resize(6);
    const auto length = static_cast<std::uint16_t>(most_token + 1);
    long_token.push_back(static_cast<std::byte>(length & 0xFFU));
    long_token.push_back(static_cast<std::byte>(length >> 8U));
    long_token.resize(long_token.size() + length);
    CHECK(decode(long_token).error().code == ErrorCode::OutOfRange);
}

TEST_CASE("a long refusal is cut to what can be sent", "[protocol]") {
    const Refused refused{
        .reason = Refusal::Token, .version = version, .detail = std::string(most_detail * 2, 'x')};
    const auto read = decode(encode(refused));
    REQUIRE(read);
    CHECK(std::get<Refused>(*read).detail == std::string(most_detail, 'x'));
}

TEST_CASE("a refusal says which side to update", "[protocol]") {
    CHECK(explain({.reason = Refusal::Version, .version = 4, .detail = ""}, 3) ==
          "the server speaks protocol 4 and this client 3: update the client");
    CHECK(explain({.reason = Refusal::Version, .version = 2, .detail = ""}, 3) ==
          "the server speaks protocol 2 and this client 3: the server is older than this client");
    CHECK(explain({.reason = Refusal::Token, .version = 3, .detail = "expired"}, 3) ==
          "the server didn't accept the login: expired");
    CHECK(explain({.reason = Refusal::Silent, .version = 3, .detail = "no hello"}, 3) ==
          "the server closed the connection: no hello");
}

TEST_CASE("positions are sent to the millimetre", "[protocol]") {
    const hog::WorldPosition at{hog::DVec3{6'371'000.0004, -0.0006, 1.2345}};
    CHECK(to_millimetres(at) == Millimetres{6'371'000'000, -1, 1'235});
    CHECK(to_millimetres(to_world(far_out)) == far_out);
}

TEST_CASE("a small move takes few bytes", "[protocol]") {
    const Snapshot moved{.tick = 1'000,
                         .baseline = 997,
                         .entered = {},
                         .changed = {{.id = NetId{12}, .moved_by = Millimetres{900, -900, 600}}},
                         .left = {}};
    // Kind, tick, baseline, three counts, then the id, what changed and three axes of two bytes.
    CHECK(encode(moved).size() == 2 + 4 + 1 + 3 + 1 + 1 + 6);
}

TEST_CASE("a snapshot's ids must go up", "[protocol]") {
    // Two changed entities, the second 0 past the first.
    const auto twice = decode(snapshot_with({2, 3, 0, 0, 0, 0}));
    REQUIRE_FALSE(twice);
    CHECK(twice.error().code == ErrorCode::Parse);
    // One leaving, past the largest id.
    CHECK(decode(snapshot_with({0, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0x10})).error().code ==
          ErrorCode::Parse);
}

TEST_CASE("snapshots that can't be right are refused", "[protocol]") {
    // A baseline 11 ticks before tick 10.
    CHECK(decode(bytes({6, 0, 10, 0, 0, 0, 11, 0, 0, 0})).error().code == ErrorCode::Parse);
    // A change of a kind this version doesn't know.
    CHECK(decode(snapshot_with({1, 1, 2, 0})).error().code == ErrorCode::Parse);
    // More entities changed than there are bytes for.
    CHECK(decode(snapshot_with({100, 1, 0})).error().code == ErrorCode::Parse);
    // A name too long.
    auto named = bytes({6, 0, 10, 0, 0, 0, 0, 1, 1, 0, 0, 0, static_cast<int>(most_name + 1)});
    named.resize(named.size() + most_name + 1 + 2, std::byte{'n'});
    CHECK(decode(named).error().code == ErrorCode::OutOfRange);
}
