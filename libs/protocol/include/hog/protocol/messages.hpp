#pragma once

#include <hog/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

// What the client and the game server say over their connection. Every message starts with its
// kind as a u16, little-endian throughout.
namespace hog::protocol {

// Raised whenever any message changes; the two sides must speak the same one.
inline constexpr std::uint32_t version = 1;
// The longest connect token a client may present; more is a broken or hostile client.
inline constexpr std::size_t most_token = 1'024;
// The longest line of text a refusal carries.
inline constexpr std::size_t most_detail = 512;

// Client to server, before anything else. Its kind and version are where they are in every
// version, so a server can answer a client of any version; of another version, only the version
// is read.
struct Hello {
    std::uint32_t version = protocol::version;
    std::vector<std::byte> token; // from the login service

    friend bool operator==(const Hello&, const Hello&) = default;
};

// Server to client: welcome in.
struct Welcome {
    std::uint32_t version = protocol::version;

    friend bool operator==(const Welcome&, const Welcome&) = default;
};

enum class Refusal : std::uint8_t {
    Version,    // the two speak different versions
    Token,      // the connect token isn't one the server accepts
    Unexpected, // something other than a readable Hello came first
    Silent,     // nothing came in time
};

// Server to client, before closing: why it won't let the client in.
struct Refused {
    Refusal reason = Refusal::Unexpected;
    std::uint32_t version = protocol::version; // the server's
    std::string detail;

    friend bool operator==(const Refused&, const Refused&) = default;
};

using Message = std::variant<Hello, Welcome, Refused>;

[[nodiscard]] std::vector<std::byte> encode(const Message& message);
// Fails on an unknown kind, a message cut short or with bytes left over, or one too long.
[[nodiscard]] Result<Message> decode(std::span<const std::byte> bytes);

// A refusal as a person reads it, on a client speaking `own` version.
[[nodiscard]] std::string explain(const Refused& refused, std::uint32_t own = version);

} // namespace hog::protocol
