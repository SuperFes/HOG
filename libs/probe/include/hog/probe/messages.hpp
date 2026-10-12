#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// What debug tools and a running server say to each other. Every message travels in a frame: a
// u32 payload length, a u8 kind, then the payload, little-endian throughout.
namespace hog::probe {

inline constexpr std::uint16_t protocol_version = 3;
// Large enough for every layer of a level 7 grid; anything bigger is a broken or hostile peer.
inline constexpr std::uint32_t max_payload = 64U << 20U;

// A per-cell field the server can send, such as "weather/temperature".
struct LayerInfo {
    std::string name;
    std::string unit;

    friend bool operator==(const LayerInfo&, const LayerInfo&) = default;
};

// One value per cell of the server's grid, in cell index order.
struct Layer {
    std::string name;
    std::vector<float> values;

    friend bool operator==(const Layer&, const Layer&) = default;
};

// How the server's clock is running.
struct Clock {
    bool paused = false;
    std::uint32_t scale = 1; // game seconds per real second

    friend bool operator==(const Clock&, const Clock&) = default;
};

// Tool to server, first: the protocol it speaks.
struct Hello {
    std::uint16_t version = protocol_version;

    friend bool operator==(const Hello&, const Hello&) = default;
};

// Server to tool, answering Hello: the world and the layers it can send.
struct Welcome {
    std::uint16_t version = protocol_version;
    std::uint64_t world_id = 0;
    std::uint8_t level = 0; // of the cell grid every layer is on
    double planet_radius = 0.0;
    std::vector<LayerInfo> layers;
    bool controllable = false; // whether tools may change the clock
    GameDuration day_length{}; // of the planet's own day, for where the sun is
    double axial_tilt = 0.0;   // degrees

    friend bool operator==(const Welcome&, const Welcome&) = default;
};

// Server to tool, before closing: why it won't talk.
struct Refused {
    std::string reason;

    friend bool operator==(const Refused&, const Refused&) = default;
};

// Tool to server: the layers it wants now, by name.
struct Fetch {
    std::vector<std::string> names;

    friend bool operator==(const Fetch&, const Fetch&) = default;
};

// Server to tool, answering Fetch: the layers as of a game time, leaving out names it doesn't know.
struct Layers {
    GameTime time;
    Clock clock;
    std::vector<Layer> layers;

    friend bool operator==(const Layers&, const Layers&) = default;
};

enum class Action : std::uint8_t {
    Pause = 1,
    Resume = 2,
    Step = 3,     // `amount` ticks, while paused
    Speed = 4,    // to `amount` game seconds per real second
    RealTime = 5, // catch up to real time, if behind it
};

// Tool to server: change how the clock runs, if the server allows it.
struct Control {
    Action action = Action::Pause;
    std::uint32_t amount = 0;

    friend bool operator==(const Control&, const Control&) = default;
};

// Server to tool, answering Control: the clock afterwards, and why not if it didn't change.
struct Controlled {
    Clock clock;
    std::string error;

    friend bool operator==(const Controlled&, const Controlled&) = default;
};

// Tool to server: the generated ground under part of the planet, sampled at the centres of a grid
// of equal latitude and longitude steps, as finely as the samples are apart.
struct Survey {
    double west = -180.0; // degrees of longitude
    double east = 180.0;
    double north = 90.0; // degrees of latitude
    double south = -90.0;
    std::uint16_t width = 0; // samples across
    std::uint16_t height = 0;
    bool underground = false; // whether to send what is at `depth`, which costs more
    float depth = 0.0F;       // metres below the surface

    friend bool operator==(const Survey&, const Survey&) = default;
};

// Server to tool, answering Survey: one value per sample, row by row from the north-west.
struct Ground {
    Survey survey;
    std::vector<float> elevation;      // metres above sea level
    std::vector<std::uint8_t> biome;   // planet::Biome
    std::vector<std::uint8_t> rock;    // planet::Rock at the depth; empty unless underground
    std::vector<std::uint8_t> deposit; // the rarest planet::Resource there plus one, or 0; likewise

    friend bool operator==(const Ground&, const Ground&) = default;
};

using Message =
    std::variant<Hello, Welcome, Refused, Fetch, Layers, Control, Controlled, Survey, Ground>;

// The whole frame for a message.
[[nodiscard]] std::vector<std::byte> encode(const Message& message);

// Splits a byte stream back into messages, however it arrives.
class FrameReader {
public:
    void feed(std::span<const std::byte> bytes);

    // The next whole message, or none until more arrives. After a failure the stream can't be
    // trusted, so every later call fails too.
    [[nodiscard]] Result<std::optional<Message>> next();

    // Bytes received but not yet read as a message.
    [[nodiscard]] std::size_t pending() const noexcept { return buffer_.size() - start_; }

private:
    std::vector<std::byte> buffer_;
    std::size_t start_ = 0;
    std::optional<Error> error_;
};

} // namespace hog::probe
