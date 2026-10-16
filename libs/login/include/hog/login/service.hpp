#pragma once

#include <hog/core/result.hpp>
#include <hog/login/keys.hpp>
#include <hog/login/token.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// What a client and the login service say: the client asks once, and is answered before the
// service closes. Each message starts with its kind as a u16, little-endian throughout.
namespace hog::login {

inline constexpr std::uint32_t login_version = 1;

// Client to login service.
struct Ask {
    std::uint32_t version = login_version;
    std::string name;

    friend bool operator==(const Ask&, const Ask&) = default;
};

// Login service to client: a token for the game server.
struct Granted {
    std::vector<std::byte> token;

    friend bool operator==(const Granted&, const Granted&) = default;
};

// Login service to client: why not.
struct Denied {
    std::string reason;

    friend bool operator==(const Denied&, const Denied&) = default;
};

using LoginMessage = std::variant<Ask, Granted, Denied>;

[[nodiscard]] std::vector<std::byte> encode(const LoginMessage& message);
[[nodiscard]] Result<LoginMessage> decode(std::span<const std::byte> bytes);

// A name the stub lets in: 1 to most_name bytes of letters, digits, spaces, '_' and '-', not
// starting or ending with a space.
[[nodiscard]] bool fit_name(std::string_view name) noexcept;

// The stub's answer to what a client sent: a token for any fit name, whose account is the name's
// hash, valid for token_life from `now`.
[[nodiscard]] LoginMessage answer(std::span<const std::byte> asked, const KeyPair& keys,
                                  UnixTime now, const Seed& nonce);

} // namespace hog::login
