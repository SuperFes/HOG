#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>

#include <array>
#include <chrono>
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
inline constexpr std::uint32_t version = 11;
// The longest connect token a client may present; more is a broken or hostile client.
inline constexpr std::size_t most_token = 1'024;
// The longest line of text a refusal carries.
inline constexpr std::size_t most_detail = 512;
// The longest name an entity is sent with.
inline constexpr std::size_t most_name = 32;
// The longest line of chat a player says.
inline constexpr std::size_t most_said = 256;
// The finest cube-sphere level the weather is sent at, which keeps it to tens of kilobytes.
inline constexpr std::uint8_t most_weather_level = 5;
// Real time between the snapshots a client is sent, whatever the server's tick rate.
inline constexpr std::chrono::milliseconds snapshot_interval{50};

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

// Client to server: where the client has moved the player's feet to. Moves count up from 1, so
// one overtaken on the way is left out; `corrections` is how many Corrected the client has taken,
// so moves it made before the newest reached it are left out too.
struct Moved {
    std::uint32_t sequence = 0;
    std::uint32_t corrections = 0;
    Millimetres at{0};

    friend bool operator==(const Moved&, const Moved&) = default;
};

// Server to client: a move it wouldn't take. The player's feet go back to `at`, and this is the
// connection's correction number `count`, from 1.
struct Corrected {
    std::uint32_t count = 0;
    Millimetres at{0};

    friend bool operator==(const Corrected&, const Corrected&) = default;
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

// A system of waves over a cell, as hog::waves takes one. Sent its height as its square root to
// the nearest 56th, so 20 metres at most; the time from crest to crest to a tenth of a second, at
// most 25.5; and its heading to a 256th of a turn.
struct CellWaves {
    float height = 0.0F;      // significant, metres
    float peak = 0.0F;        // rad/s; 0 where there are none
    Vec2 heading{1.0F, 0.0F}; // unit (east, north), the way they run

    friend bool operator==(const CellWaves&, const CellWaves&) = default;
};

// The weather over a cell, as much of it as is drawn. It's sent a byte each, so only near what's
// given: cloud and falling as their square roots to the nearest 64th and 16th, so the lightest keep
// their detail; snow and humidity to a 255th; drift to the metre a second and the wind to half
// of one; the fog's top to 32 metres and the cloud's base and top to 64; the sea's current to a
// 32nd of a metre a second; each clamped to its byte.
struct CellWeather {
    float cloud = 0.0F;    // mm of condensed water, under 16
    float falling = 0.0F;  // rain and snow, mm an hour, under 255
    float snow = 0.0F;     // share of what falls that's snow, 0 to 1
    Vec2 drift{0.0F};      // (east, north) m/s the weather moves with, within ±127
    Vec2 wind{0.0F};       // (east, north) m/s ten metres up, within ±63.5
    float humidity = 0.0F; // as sim::Sky's
    // Metres above sea level, as sim::Sky's.
    float fog_top = 0.0F; // under 8160
    float base = 0.0F;    // under 16320
    float top = 0.0F;
    // The sea's wind sea, its two strongest swells and its current, (east, north) m/s within ±4.
    CellWaves wind_sea{};
    std::array<CellWaves, 2> swells{};
    Vec2 current{0.0F};

    friend bool operator==(const CellWeather&, const CellWeather&) = default;
};

// Server to client: the weather over the whole planet, a cell for each cube-sphere cell at
// `level`, in sim::CellGrid's order. What the client shows fades to it from what it showed over
// `fade` of real time.
struct Weather {
    std::uint8_t level = 0; // at most most_weather_level
    std::chrono::milliseconds fade{0};
    std::vector<CellWeather> cells; // 6 × 4^level of them

    friend bool operator==(const Weather&, const Weather&) = default;
};

// How a cell's woods fare, as flora::Habitat takes them, each a share of the trees its ground
// would hold, and the share of its ground a fire left black. Sent a byte each, to a 255th.
struct CellWoodland {
    float living = 1.0F;
    float established = 1.0F;
    float failing = 0.0F;
    float dead = 0.0F;
    float charred = 0.0F;
    float scar = 0.0F;

    friend bool operator==(const CellWoodland&, const CellWoodland&) = default;
};

// Server to client: how the woods fare over the whole planet, a cell for each cube-sphere cell at
// `level`, in sim::CellGrid's order. Sent when it changes, and on joining.
struct Woodlands {
    std::uint8_t level = 0;          // at most most_weather_level
    std::vector<CellWoodland> cells; // 6 × 4^level of them

    friend bool operator==(const Woodlands&, const Woodlands&) = default;
};

// A line of chat. A client says it with `from` empty; the server sends it on to every player,
// the speaker included so all read the same order, with the speaker's name it holds itself
// rather than any the client claims. Neither part carries control characters.
struct Said {
    std::string from; // at most most_name bytes
    std::string text; // at most most_said bytes

    friend bool operator==(const Said&, const Said&) = default;
};

// Client to server: run the world's clock at `scale` game seconds a real one, to watch the world
// go by. The server obliges only on a world whose config allows time control.
struct Hurry {
    std::uint32_t scale = 1;

    friend bool operator==(const Hurry&, const Hurry&) = default;
};

// Server to client: how fast the world's clock runs, sent to everyone whose world sped up or
// slowed down, and on joining one that isn't at real time.
struct Clock {
    std::uint32_t scale = 1;

    friend bool operator==(const Clock&, const Clock&) = default;
};

// How many cells the weather at `level` has.
[[nodiscard]] constexpr std::size_t weather_cells(std::uint8_t level) noexcept {
    return std::size_t{6} << (2U * level);
}

// As a cell's weather reads back once sent.
[[nodiscard]] CellWeather as_sent(const CellWeather& cell) noexcept;
// As a cell's woods read back once sent.
[[nodiscard]] CellWoodland as_sent(const CellWoodland& cell) noexcept;

using Message = std::variant<Hello, Welcome, Refused, Moved, Ack, Snapshot, Corrected, Weather,
                             Said, Hurry, Clock, Woodlands>;

[[nodiscard]] std::vector<std::byte> encode(const Message& message);
// Fails on an unknown kind, a message cut short or with bytes left over, one too long, a snapshot
// out of order, weather or woods too fine or with too few or many cells, or chat with control
// characters.
[[nodiscard]] Result<Message> decode(std::span<const std::byte> bytes);

// A refusal as a person reads it, on a client speaking `own` version.
[[nodiscard]] std::string explain(const Refused& refused, std::uint32_t own = version);

} // namespace hog::protocol
