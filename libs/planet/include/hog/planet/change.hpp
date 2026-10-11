#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/climate.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace hog::planet {

// Who made a change: a player, a game master or a world system.
enum class AuthorId : std::uint64_t {};

// Where a change sits over the procedural base; higher layers win.
enum class Layer : std::uint8_t {
    Authored,
    Simulation,
    Player,
};

inline constexpr int layer_count = static_cast<int>(Layer::Player) + 1;

[[nodiscard]] std::string_view to_string(Layer layer) noexcept;

// Fixed-point metres above sea level.
struct SetElevation {
    std::int64_t elevation = 0;

    friend bool operator==(const SetElevation&, const SetElevation&) = default;
};

// Digs the ground down by a positive fixed-point depth in metres, taking whatever rock and
// deposits lay there.
struct Excavate {
    std::int64_t depth = 0;

    friend bool operator==(const Excavate&, const Excavate&) = default;
};

struct SetBiome {
    Biome biome = Biome::Ocean;

    friend bool operator==(const SetBiome&, const SetBiome&) = default;
};

// Alternatives are only ever appended: a kind is the payload's index, and is stored.
using ChangePayload = std::variant<SetElevation, Excavate, SetBiome>;

enum class ChangeKind : std::uint8_t {
    SetElevation,
    Excavate,
    SetBiome,
};

inline constexpr int change_kind_count = static_cast<int>(std::variant_size_v<ChangePayload>);

[[nodiscard]] std::string_view to_string(ChangeKind kind) noexcept;

[[nodiscard]] constexpr ChangeKind kind_of(const ChangePayload& payload) noexcept {
    return static_cast<ChangeKind>(payload.index());
}

// One permanent change to one cell, made at a moment of game time by an author.
struct Change {
    CellId cell;
    GameTime time;
    AuthorId author{};
    Layer layer = Layer::Player;
    ChangePayload payload;

    [[nodiscard]] ChangeKind kind() const noexcept { return kind_of(payload); }

    friend bool operator==(const Change&, const Change&) = default;
};

// Selects changes, for rolling back or listing. Every set field must match.
struct ChangeFilter {
    std::optional<AuthorId> author;
    std::optional<Layer> layer;
    // Game time from `from` up to, not including, `until`.
    GameTime from = GameTime::min();
    GameTime until = GameTime::max();

    [[nodiscard]] bool matches(const Change& change) const noexcept;
};

// A payload's bytes for storage: fixed-width little-endian fields, the same on every platform.
[[nodiscard]] std::vector<std::byte> encode_payload(const ChangePayload& payload);

// The payload of a stored kind; fails on an unknown kind or malformed bytes.
[[nodiscard]] Result<ChangePayload> decode_payload(std::uint8_t kind,
                                                   std::span<const std::byte> bytes);

} // namespace hog::planet
