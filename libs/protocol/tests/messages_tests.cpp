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

std::vector<Message> examples() {
    return {Hello{}, Hello{.version = version, .token = bytes({1, 2, 3, 250})}, Welcome{},
            Refused{.reason = Refusal::Version, .version = 7, .detail = "too new"},
            Refused{.reason = Refusal::Silent, .version = version, .detail = ""}};
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
