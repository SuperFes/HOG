#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// What the client and the game server say over their connection. Every message starts with its
// kind as a u16, little-endian throughout.
namespace hog::protocol {

// Raised whenever any message changes; the two sides must speak the same one.
inline constexpr std::uint32_t version = 2;
// The longest connect token a client may present; more is a broken or hostile client.
inline constexpr std::size_t most_token = 1'024;
// The longest line of text a refusal carries.
inline constexpr std::size_t most_detail = 512;
// The longest name an entity is sent with.
inline constexpr std::size_t most_name = 32;

// Positions as they're sent: whole millimetres from the planet's centre.
using Millimetres = I64Vec3;

[[nodiscard]] Millimetres to_millimetres(WorldPosition position) noexcept;
[[nodiscard]] WorldPosition to_world(Millimetres millimetres) noexcept;

// An entity as both ends know it while it's replicated; 0 is never one.
enum class NetId : std::uint32_t {};

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

// Client to server, until movement is the server's: where the player is.
struct Moved {
    Millimetres at{0};

    friend bool operator==(const Moved&, const Moved&) = default;
};

// Client to server: the newest snapshot it has applied, for the next to be coded against.
struct Ack {
    std::uint32_t tick = 0;

    friend bool operator==(const Ack&, const Ack&) = default;
};

// An entity coming into view, whole.
struct Entered {
    NetId id{};
    Millimetres at{0};
    std::string name;

    friend bool operator==(const Entered&, const Entered&) = default;
};

// What changed about an entity in view since the baseline.
struct Changed {
    NetId id{};
    std::optional<Millimetres> moved_by;

    friend bool operator==(const Changed&, const Changed&) = default;
};

// Server to client: what's in view at a tick, as changes to the baseline it's coded against, or
// to nothing. Each list is in order of id, with no id twice. Snapshots count their own ticks,
// from 1 at a connection's first, so they don't run out.
struct Snapshot {
    std::uint32_t tick = 0;
    std::optional<std::uint32_t> baseline; // before tick
    std::vector<Entered> entered;
    std::vector<Changed> changed;
    std::vector<NetId> left;

    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

using Message = std::variant<Hello, Welcome, Refused, Moved, Ack, Snapshot>;

[[nodiscard]] std::vector<std::byte> encode(const Message& message);
// Fails on an unknown kind, a message cut short or with bytes left over, one too long, or a
// snapshot out of order.
[[nodiscard]] Result<Message> decode(std::span<const std::byte> bytes);

// A refusal as a person reads it, on a client speaking `own` version.
[[nodiscard]] std::string explain(const Refused& refused, std::uint32_t own = version);

} // namespace hog::protocol
