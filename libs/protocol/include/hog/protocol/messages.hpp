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
inline constexpr std::uint32_t version = 29;
// The longest planet a welcome carries.
inline constexpr std::size_t most_planet = 1'024;
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

// Server to client: welcome in. `world` is the id of the planet it runs, which a client that makes
// ground itself checks is the planet it makes; 0 for none given.
struct Welcome {
    std::uint32_t version = protocol::version;
    std::uint64_t world = 0;
    // The planet it runs, as hog::globe::encode_planet has it, for a client that draws it; at most
    // most_planet bytes, none for none.
    std::vector<std::byte> planet;

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

// How a player gets about: on foot, or flying freely to look at the world.
enum class Stance : std::uint8_t { Walking, Flying };

// Which way a player looks, in 65536ths of a turn: `heading` from north towards east along the
// ground, and `pitch` up from the horizon.
struct Facing {
    std::uint16_t heading = 0;
    std::int16_t pitch = 0;

    friend bool operator==(const Facing&, const Facing&) = default;
};

// Client to server: where the client has moved the player's feet to, or their eye while flying.
// Moves count up from 1, so one overtaken on the way is left out; `corrections` is how many
// Corrected the client has taken, so moves it made before the newest reached it are left out too.
struct Moved {
    std::uint32_t sequence = 0;
    std::uint32_t corrections = 0;
    Millimetres at{0};
    Stance stance = Stance::Walking;
    Facing facing{};

    friend bool operator==(const Moved&, const Moved&) = default;
};

// Server to client: a move it wouldn't take. The player's feet go back to `at`, and this is the
// connection's correction number `count`, from 1.
struct Corrected {
    std::uint32_t count = 0;
    Millimetres at{0};

    friend bool operator==(const Corrected&, const Corrected&) = default;
};

// Server to client, on joining: where the player was when they last left, and how. A walker is
// stood on the ground under `at`, which may have changed since.
struct Placed {
    Millimetres at{0};
    Stance stance = Stance::Walking;
    Facing facing{};

    friend bool operator==(const Placed&, const Placed&) = default;
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
    float cover = 0.0F;    // share of the sky the cloud covers, 0 to 1, to a 255th
    float falling = 0.0F;  // rain and snow, mm an hour, under 255
    float snow = 0.0F;     // share of what falls that's snow, 0 to 1
    Vec2 drift{0.0F};      // (east, north) m/s the weather moves over the ground, within ±127
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
    // Game seconds the weather ran for each real second since the last, to a thousandth: how much
    // faster than the wind's own speed the cloud is carried, as the clock is hurried.
    float pace = 1.0F;
    std::vector<CellWeather> cells; // 6 × 4^level of them

    friend bool operator==(const Weather&, const Weather&) = default;
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

// The widest ball a player may dig or build at once, in millimetres.
inline constexpr std::uint32_t most_edit = 8'000;
// Bytes a brick's coding may take, and bricks a message may carry: well within what's sent.
inline constexpr std::size_t most_brick = std::size_t{12} * 1024;
inline constexpr std::size_t most_bricks = 40;

// Client to server: take shaped matter out of a ball at `centre`, millimetres from the planet's
// centre, `radius` millimetres across, over most_edit refused. The server obliges where the
// player may, and sends back the bricks it changed.
struct Dug {
    Millimetres centre{0};
    std::uint32_t radius = 0;

    friend bool operator==(const Dug&, const Dug&) = default;
};

// Client to server: build a ball of `material`, as hog::globe numbers them, the same way.
struct Built {
    Millimetres centre{0};
    std::uint32_t radius = 0;
    std::uint8_t material = 0;

    friend bool operator==(const Built&, const Built&) = default;
};

// A brick of the voxel world as the store codes it, by its key.
struct SentBrick {
    std::uint64_t key = 0;
    std::vector<std::byte> bytes; // at most most_brick

    friend bool operator==(const SentBrick&, const SentBrick&) = default;
};

// Server to client: bricks changed from what the generator says, to hold in their place, as
// many as fit a message at a time; sent bulk, so they never hold up moves.
struct Bricks {
    std::vector<SentBrick> bricks; // at most most_bricks

    friend bool operator==(const Bricks&, const Bricks&) = default;
};

// Grain in flight at most in a message; a client is told of the nearest to it.
inline constexpr std::size_t most_grains = 256;

// A lump of loose grain falling, bouncing or rolling, as the server's flow has it.
struct Flying {
    NetId id{};
    Millimetres at{0};
    std::array<std::int16_t, 3> velocity{}; // centimetres a second
    std::uint8_t material = 0;              // as hog::globe numbers them
    std::uint32_t amount = 0;               // of a voxel's own volume, in 32768ths

    friend bool operator==(const Flying&, const Flying&) = default;
};

// Server to client, unreliable: all the grain in flight near the client at the server's tick, in
// order of id. What a client held that isn't listed has landed, and is in the bricks sent. Between
// two of these it carries each on under gravity, so grain is seen to travel, not jump.
struct Grains {
    std::uint32_t tick = 0;
    std::vector<Flying> flying; // at most most_grains

    friend bool operator==(const Grains&, const Grains&) = default;
};

// Server to client, unreliable: how high the ground stands over the planet's radius, in metres,
// under the client's player, for the client to flatten the planet's curve about it.
struct Ground {
    float height = 0.0F;

    friend bool operator==(const Ground&, const Ground&) = default;
};

// The most a chunk's meshes are sent with: a chunk's ground is meshed in some thousands of vertices,
// most of them under 6,000, and its still water in few; a message carries 512 KiB.
inline constexpr std::size_t most_ground_vertices = 14'000;
inline constexpr std::size_t most_ground_indices = 42'000;
inline constexpr std::size_t most_water_vertices = 3'000;
inline constexpr std::size_t most_water_indices = 9'000;

// A corner of a surface, as terrain::BlockVertex has it, its normal sent to a 32767th.
struct MeshVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<std::uint8_t, 3> tiles{};
    std::uint8_t corner = 0;

    friend bool operator==(const MeshVertex&, const MeshVertex&) = default;
};

// Triangles of three indices each, anticlockwise seen from outside; positions are metres from
// `origin`.
struct SentMesh {
    Millimetres origin{0};
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices; // each below the vertices' count

    friend bool operator==(const SentMesh&, const SentMesh&) = default;
};

// Server to client, bulk: a chunk of the planet's surface to show under its key, in place of any
// shown there: its ground, and the still water standing on it.
//
// One that's too big for a message is sent in `parts`, each a piece of the ground, `part` counting
// from 0; the water comes with part 0. All of a key's parts are one chunk, shown once all are there.
struct Chunk {
    std::uint64_t key = 0;
    // Which meshing of the chunk this is: it goes up each time it's meshed again, as the world
    // under it changes.
    std::uint32_t version = 0;
    std::uint8_t part = 0;
    std::uint8_t parts = 1;
    SentMesh ground;
    SentMesh water;

    friend bool operator==(const Chunk&, const Chunk&) = default;
};

// A chunk wanted, and the newest meshing of it there is, 0 while it isn't meshed yet.
struct PlannedChunk {
    std::uint64_t key = 0;
    std::uint32_t version = 0;

    friend bool operator==(const PlannedChunk&, const PlannedChunk&) = default;
};

// Chunks in a plan, at most: those wanted round a player, nearest first.
inline constexpr std::size_t most_planned = 4'096;

// Server to client, bulk: the chunks to show round the client now, nearest first, as often as that
// changes. The client keeps those it has, asks for those it hasn't, and draws the plan from them;
// the server never takes a chunk back.
struct ChunkPlan {
    std::vector<PlannedChunk> wanted; // at most most_planned

    friend bool operator==(const ChunkPlan&, const ChunkPlan&) = default;
};

// The falloff of detail the client asks for, within these.
// A chunk must be drawn further off than it's wide, so the ratio is over 1.
inline constexpr float least_detail_ratio = 1.25F;
inline constexpr float most_detail_ratio = 8.0F;
inline constexpr float least_detail_slope = 1.0F;
inline constexpr float most_detail_slope = 3.0F;
inline constexpr float least_detail_near = 50.0F;    // metres
inline constexpr float most_detail_near = 20'000.0F; // metres

// Client to server, reliable, once welcomed and when it changes: how fast the client wants detail
// to fall off with distance. Each chunk is as coarse as keeps it `ratio` of its widths from the
// eye, as far as `near` metres; past that, a `slope` over 1 has detail fall off faster, that many
// levels to a doubling of distance, so the ground a long way off is coarser and the client is sent
// fewer chunks, while what's near stays as it was.
struct ViewDetail {
    float ratio = 3.0F;
    float near = 1'000.0F;
    float slope = 1.0F;

    friend bool operator==(const ViewDetail&, const ViewDetail&) = default;
};

// Chunks asked for in a message, at most.
inline constexpr std::size_t most_asked = 256;

// Client to server, reliable: chunks of the plan the client lacks, nearest first, to be sent when
// meshed; asked for again if they don't come.
struct ChunksAsked {
    std::vector<std::uint64_t> keys; // at most most_asked

    friend bool operator==(const ChunksAsked&, const ChunksAsked&) = default;
};

// How many cells the weather at `level` has.
[[nodiscard]] constexpr std::size_t weather_cells(std::uint8_t level) noexcept {
    return std::size_t{6} << (2U * level);
}

// As a cell's weather reads back once sent.
[[nodiscard]] CellWeather as_sent(const CellWeather& cell) noexcept;

using Message = std::variant<Hello, Welcome, Refused, Moved, Ack, Snapshot, Corrected, Weather,
                             Said, Hurry, Clock, Placed, Dug, Built, Bricks, Grains, Chunk, Ground, ChunkPlan, ChunksAsked, ViewDetail>;

[[nodiscard]] std::vector<std::byte> encode(const Message& message);
// Fails on an unknown kind, a message cut short or with bytes left over, one too long, a snapshot
// out of order, weather or woods too fine or with too few or many cells, chat with control
// characters, a stance it doesn't know, an edit of no width or too wide, or bricks too many or
// too big.
[[nodiscard]] Result<Message> decode(std::span<const std::byte> bytes);

// A refusal as a person reads it, on a client speaking `own` version.
[[nodiscard]] std::string explain(const Refused& refused, std::uint32_t own = version);

} // namespace hog::protocol
