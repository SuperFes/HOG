#include <hog/core/hash.hpp>
#include <hog/login/service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

using hog::ErrorCode;
using namespace hog::login;
using namespace std::chrono_literals;

namespace {

constexpr UnixTime now{std::chrono::sys_days{std::chrono::year{2026} / 10 / 16}};

KeyPair keys() {
    Seed seed{};
    seed.fill(9);
    return key_pair(seed);
}

Seed nonce() {
    Seed seed{};
    seed.fill(3);
    return seed;
}

std::string denial(const LoginMessage& message) {
    const auto* denied = std::get_if<Denied>(&message);
    REQUIRE(denied != nullptr);
    return denied != nullptr ? denied->reason : std::string{};
}

} // namespace

TEST_CASE("login messages read back as written", "[login]") {
    const std::vector<LoginMessage> examples{
        Ask{.version = login_version, .name = "Matt"},
        Granted{.token = {std::byte{1}, std::byte{2}}},
        Denied{.reason = "no"},
    };
    for (const LoginMessage& message : examples) {
        const auto read = decode(encode(message));
        REQUIRE(read);
        CHECK(*read == message);
    }
    CHECK(decode(std::vector<std::byte>{std::byte{9}, std::byte{0}}).error().code ==
          ErrorCode::Unsupported);
    auto cut = encode(Ask{.version = login_version, .name = "Matt"});
    cut.pop_back();
    CHECK(decode(cut).error().code == ErrorCode::Parse);
}

TEST_CASE("names are letters, digits, spaces, '_' and '-'", "[login]") {
    CHECK(fit_name("Matt"));
    CHECK(fit_name("big_hog-2 too"));
    CHECK_FALSE(fit_name(""));
    CHECK_FALSE(fit_name(" Matt"));
    CHECK_FALSE(fit_name("Matt "));
    CHECK_FALSE(fit_name("Ma;tt"));
    CHECK_FALSE(fit_name(std::string(most_name + 1, 'a')));
    CHECK(fit_name(std::string(most_name, 'a')));
}

TEST_CASE("the stub grants a token for a fit name", "[login]") {
    const KeyPair signer = keys();
    const LoginMessage granted =
        answer(encode(Ask{.version = login_version, .name = "Matt"}), signer, now, nonce());
    REQUIRE(std::holds_alternative<Granted>(granted));
    const auto token = check(std::get<Granted>(granted).token, signer.public_key, now + 1s);
    REQUIRE(token);
    CHECK(token->name == "Matt");
    CHECK(token->account == hog::hash_string("Matt"));
    CHECK(token->expires == now + token_life);
    CHECK(token->nonce[0] == 3);
}

TEST_CASE("the stub denies what it can't grant, saying why", "[login]") {
    const KeyPair signer = keys();
    CHECK(
        denial(answer(encode(Ask{.version = login_version, .name = "M;tt"}), signer, now, nonce()))
            .contains("isn't a name"));
    CHECK(denial(answer(encode(Ask{.version = login_version + 1, .name = "Matt"}), signer, now,
                        nonce()))
              .contains("version"));
    CHECK(denial(answer(encode(Denied{.reason = "?"}), signer, now, nonce())).contains("ask"));
    CHECK(denial(answer(std::vector<std::byte>{std::byte{1}}, signer, now, nonce()))
              .contains("couldn't read"));
}
