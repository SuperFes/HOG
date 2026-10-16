#include <hog/protocol/handshake.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace hog::protocol;
using namespace std::chrono_literals;

namespace {

constexpr hog::MonotonicClock::time_point opened{1'000s};
constexpr hog::login::UnixTime now{std::chrono::sys_days{std::chrono::year{2026} / 10 / 16}};

hog::login::KeyPair service_keys(std::uint8_t fill) {
    hog::login::Seed seed{};
    seed.fill(fill);
    return hog::login::key_pair(seed);
}

std::vector<std::byte> token_for(const std::string& name, const hog::login::KeyPair& keys,
                                 hog::login::UnixTime issued) {
    return hog::login::sign({.account = 1,
                             .name = name,
                             .issued = issued,
                             .expires = issued + hog::login::token_life,
                             .nonce = {}},
                            keys);
}

Refusal refusal_of(const Answer& answer) {
    REQUIRE(answer.reply.has_value());
    const Message reply = answer.reply.value_or(Message{});
    const auto* refused = std::get_if<Refused>(&reply);
    REQUIRE(refused != nullptr);
    return refused != nullptr ? refused->reason : Refusal::Unexpected;
}

} // namespace

TEST_CASE("a hello of this version is welcomed", "[protocol][handshake]") {
    Handshake handshake{opened};
    const Answer answer = handshake.received(encode(Hello{}), now);
    CHECK(answer.reply == Message{Welcome{}});
    CHECK_FALSE(answer.close);
    CHECK(handshake.welcomed());
    CHECK_FALSE(handshake.waited(opened + 1h));
}

TEST_CASE("a hello of another version is refused, saying which", "[protocol][handshake]") {
    for (const std::uint32_t other : {version - 1, version + 1}) {
        Handshake handshake{opened};
        const Answer answer = handshake.received(encode(Hello{.version = other, .token = {}}), now);
        CHECK(answer.close);
        CHECK(refusal_of(answer) == Refusal::Version);
        CHECK(std::get<Refused>(answer.reply.value_or(Message{})).version == version);
        CHECK_FALSE(handshake.welcomed());
    }
}

TEST_CASE("anything but a readable hello first is refused", "[protocol][handshake]") {
    const std::vector<std::vector<std::byte>> firsts{
        encode(Welcome{}), {}, {std::byte{1}, std::byte{0}, std::byte{1}}, {std::byte{77}}};
    for (const auto& first : firsts) {
        Handshake handshake{opened};
        const Answer answer = handshake.received(first, now);
        CHECK(answer.close);
        CHECK(refusal_of(answer) == Refusal::Unexpected);
        CHECK_FALSE(answer.reason.empty());
    }
}

TEST_CASE("a client that doesn't say hello in time is refused", "[protocol][handshake]") {
    const Handshake handshake{opened};
    CHECK_FALSE(handshake.waited(opened));
    CHECK_FALSE(handshake.waited(opened + hello_within - 1ms));
    const auto answer = handshake.waited(opened + hello_within);
    REQUIRE(answer.has_value());
    CHECK(answer.value_or(Answer{}).close);
    CHECK(refusal_of(answer.value_or(Answer{})) == Refusal::Silent);
}

TEST_CASE("without the login service's key, anyone is welcomed", "[protocol][handshake]") {
    Handshake handshake{opened};
    CHECK(handshake.received(encode(Hello{}), now).reply == Message{Welcome{}});
    CHECK_FALSE(handshake.player());
}

TEST_CASE("with the login service's key, a hello needs its token", "[protocol][handshake]") {
    const hog::login::KeyPair keys = service_keys(1);
    SECTION("signed and in date, it's welcomed, knowing who") {
        Handshake handshake{opened, keys.public_key};
        const Answer answer = handshake.received(
            encode(Hello{.version = version, .token = token_for("Matt", keys, now)}), now);
        CHECK(answer.reply == Message{Welcome{}});
        REQUIRE(handshake.player());
        CHECK(handshake.player().value_or(hog::login::Token{}).name == "Matt");
    }
    SECTION("missing, forged or expired, it's refused") {
        const std::vector<std::vector<std::byte>> tokens{
            {},
            token_for("Matt", service_keys(2), now),
            token_for("Matt", keys, now - hog::login::token_life - 1s)};
        for (const auto& token : tokens) {
            Handshake handshake{opened, keys.public_key};
            const Answer answer =
                handshake.received(encode(Hello{.version = version, .token = token}), now);
            CHECK(answer.close);
            CHECK(refusal_of(answer) == Refusal::Token);
            CHECK_FALSE(handshake.player());
        }
    }
}
