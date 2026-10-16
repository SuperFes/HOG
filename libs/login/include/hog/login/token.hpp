#pragma once

#include <hog/core/result.hpp>
#include <hog/login/keys.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hog::login {

inline constexpr std::uint8_t token_version = 1;
inline constexpr std::size_t most_name = 32; // bytes
// How long a token lets its holder in for, from when it's issued.
inline constexpr std::chrono::seconds token_life{60};
// How far a server's clock may run behind the login service's.
inline constexpr std::chrono::seconds clock_slack{5};

using UnixTime = std::chrono::sys_seconds;

// Who a connect token lets in, and when.
struct Token {
    std::uint64_t account = 0;
    std::string name;
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    UnixTime issued{};
    // NOLINTNEXTLINE(readability-redundant-member-init): likewise
    UnixTime expires{};
    std::array<std::uint8_t, 16> nonce{};

    friend bool operator==(const Token&, const Token&) = default;
};

// The token's fields, little-endian after its version, then an Ed25519 signature of them.
[[nodiscard]] std::vector<std::byte> sign(const Token& token, const KeyPair& keys);

// The token signed in `bytes`, if it's well formed, of this version, signed by `key`, and valid
// at `now`.
[[nodiscard]] Result<Token> check(std::span<const std::byte> bytes, const PublicKey& key,
                                  UnixTime now);

} // namespace hog::login
