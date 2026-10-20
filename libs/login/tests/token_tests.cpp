#include <hog/login/keys.hpp>
#include <hog/login/token.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

using hog::ErrorCode;
using namespace hog::login;
using namespace std::chrono_literals;

namespace {

constexpr Seed seed_of(std::uint8_t fill) {
    Seed seed{};
    seed.fill(fill);
    return seed;
}

constexpr UnixTime issued{std::chrono::sys_days{std::chrono::year{2026} / 10 / 16}};

Token a_token() {
    Token token{.account = 0x1234'5678'9ABC'DEF0,
                .name = "Matt",
                .issued = issued,
                .expires = issued + token_life,
                .nonce = {}};
    token.nonce.fill(7);
    return token;
}

} // namespace

TEST_CASE("a signed token checks back as it was", "[login]") {
    const KeyPair keys = key_pair(seed_of(1));
    const auto checked = check(sign(a_token(), keys), keys.public_key, issued + 10s);
    REQUIRE(checked);
    CHECK(*checked == a_token());
}

TEST_CASE("a token changed, or signed by another key, is refused", "[login]") {
    const KeyPair keys = key_pair(seed_of(1));
    const std::vector<std::byte> good = sign(a_token(), keys);
    for (std::size_t at = 1; at < good.size(); ++at) {
        CAPTURE(at);
        std::vector<std::byte> changed = good;
        changed[at] ^= std::byte{1};
        const auto checked = check(changed, keys.public_key, issued + 10s);
        REQUIRE_FALSE(checked);
        CHECK(checked.error().code == ErrorCode::InvalidArgument);
    }
    const KeyPair other = key_pair(seed_of(2));
    CHECK(check(sign(a_token(), other), keys.public_key, issued).error().code ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("a token of another version is refused before its signature", "[login]") {
    const KeyPair keys = key_pair(seed_of(1));
    std::vector<std::byte> bytes = sign(a_token(), keys);
    bytes.front() = std::byte{token_version + 1};
    CHECK(check(bytes, keys.public_key, issued).error().code == ErrorCode::Unsupported);
    CHECK(check(std::span{bytes}.first(10), keys.public_key, issued).error().code ==
          ErrorCode::Parse);
}

TEST_CASE("a token lets its holder in only while it's valid", "[login]") {
    const KeyPair keys = key_pair(seed_of(1));
    const std::vector<std::byte> bytes = sign(a_token(), keys);
    CHECK(check(bytes, keys.public_key, issued));
    CHECK(check(bytes, keys.public_key, issued + token_life - 1s));
    CHECK(check(bytes, keys.public_key, issued + token_life).error().code == ErrorCode::OutOfRange);
    // A server's clock a little behind the login service's still takes it.
    CHECK(check(bytes, keys.public_key, issued - clock_slack));
    CHECK(check(bytes, keys.public_key, issued - clock_slack - 1s).error().code ==
          ErrorCode::OutOfRange);
}

TEST_CASE("a token's name is cut to what's allowed", "[login]") {
    const KeyPair keys = key_pair(seed_of(1));
    Token token = a_token();
    token.name = std::string(most_name + 10, 'n');
    const auto checked = check(sign(token, keys), keys.public_key, issued);
    REQUIRE(checked);
    CHECK(checked->name == std::string(most_name, 'n'));
}

TEST_CASE("a seed makes the same keys, and random seeds differ", "[login]") {
    CHECK(key_pair(seed_of(3)).public_key == key_pair(seed_of(3)).public_key);
    CHECK(key_pair(seed_of(3)).public_key != key_pair(seed_of(4)).public_key);
    const auto first = random_seed();
    const auto second = random_seed();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(*first != *second);
}

TEST_CASE("keys read back from hex", "[login]") {
    const PublicKey key = key_pair(seed_of(5)).public_key;
    const std::string text = to_hex(key);
    CHECK(text.size() == 64);
    const auto read = from_hex(text, key.size());
    REQUIRE(read);
    CHECK(std::ranges::equal(*read, key));
    CHECK(from_hex(text.substr(1), key.size()).error().code == ErrorCode::Parse);
    CHECK(from_hex(std::string(64, 'g'), key.size()).error().code == ErrorCode::Parse);
}

TEST_CASE("a login service's keys are made once and kept", "[login]") {
    const hog::testing::TemporaryDirectory directory;
    const auto seed_file = directory.path() / "var" / "login.key";
    const auto public_file = directory.path() / "var" / "login.pub";
    const auto made = load_or_make_keys(seed_file, public_file);
    REQUIRE(made);
    const auto again = load_or_make_keys(seed_file, public_file);
    REQUIRE(again);
    CHECK(again->public_key == made->public_key);
    const auto published = load_public_key(public_file);
    REQUIRE(published);
    CHECK(*published == made->public_key);

    std::ofstream{public_file} << "not a key\n";
    CHECK(load_public_key(public_file).error().code == ErrorCode::Parse);
    CHECK_FALSE(load_public_key(directory.path() / "missing.pub"));
}
