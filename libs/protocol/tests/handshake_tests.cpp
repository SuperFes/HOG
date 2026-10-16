#include <hog/protocol/handshake.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

using namespace hog::protocol;
using namespace std::chrono_literals;

namespace {

constexpr hog::MonotonicClock::time_point opened{1'000s};

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
    const Answer answer = handshake.received(encode(Hello{}));
    CHECK(answer.reply == Message{Welcome{}});
    CHECK_FALSE(answer.close);
    CHECK(handshake.welcomed());
    CHECK_FALSE(handshake.waited(opened + 1h));
}

TEST_CASE("a hello of another version is refused, saying which", "[protocol][handshake]") {
    for (const std::uint32_t other : {version - 1, version + 1}) {
        Handshake handshake{opened};
        const Answer answer = handshake.received(encode(Hello{.version = other, .token = {}}));
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
        const Answer answer = handshake.received(first);
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
