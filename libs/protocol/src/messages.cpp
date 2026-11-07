#include <hog/core/assert.hpp>
#include <hog/core/bytes.hpp>
#include <hog/protocol/messages.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace hog::protocol {

namespace {

// A u16 on the wire, leaving room for many more.
// NOLINTNEXTLINE(performance-enum-size)
enum class Kind : std::uint16_t {
    Hello = 1,
    Welcome = 2,
    Refused = 3,
    Moved = 4,
    Ack = 5,
    Snapshot = 6,
    Corrected = 7,
    Weather = 8,
    Said = 9,
    Hurry = 10,
    Clock = 11,
    Placed = 13,
    Dug = 14,
    Built = 15,
    Columns = 16,
    Grains = 17,
    Chunk = 19,
    Ground = 21,
    ChunkPlan = 22,
    ChunksAsked = 23,
    ViewDetail = 24,
}; // NOLINT(performance-enum-size)

// Which of an entity's changes follow its id in a snapshot.
constexpr std::uint8_t changed_position = 1U;
constexpr std::uint8_t known_changes = changed_position;

// Cloud and falling are sent as square roots, so thin cloud and drizzle keep their detail.
constexpr float cover_steps = 255.0F;
constexpr float pace_steps = 1'000.0F;
constexpr float cloud_steps = 64.0F;   // a byte's steps to the root of a millimetre
constexpr float falling_steps = 16.0F; // to the root of a millimetre an hour
constexpr float snow_steps = 255.0F;
constexpr float fastest_drift = 127.0F;
constexpr float humidity_steps = 255.0F;
constexpr float fog_step = 32.0F; // metres
constexpr float height_step = 64.0F;
constexpr float wave_steps = 56.0F;     // to the root of a metre
constexpr float period_steps = 10.0F;   // to the second
constexpr float heading_steps = 256.0F; // to the turn
constexpr float current_steps = 32.0F;  // to the metre a second
constexpr float wind_steps = 2.0F;
constexpr std::size_t weather_cell_bytes = 23;

[[nodiscard]] std::uint8_t to_byte(float value) noexcept {
    // Also NaN to 0.
    if (!(value > 0.0F)) {
        return 0;
    }
    return static_cast<std::uint8_t>(std::lround(std::min(value, 255.0F)));
}

[[nodiscard]] std::uint8_t rooted(float value, float steps) noexcept {
    return to_byte(std::sqrt(std::max(value, 0.0F)) * steps);
}

[[nodiscard]] float squared(std::uint8_t byte, float steps) noexcept {
    const float root = static_cast<float>(byte) / steps;
    return root * root;
}

[[nodiscard]] std::uint8_t speed(float value) noexcept {
    if (std::isnan(value)) {
        return 0;
    }
    const auto whole =
        static_cast<std::int8_t>(std::lround(std::clamp(value, -fastest_drift, fastest_drift)));
    return static_cast<std::uint8_t>(whole);
}

[[nodiscard]] float speed_of(std::uint8_t byte) noexcept {
    return static_cast<float>(static_cast<std::int8_t>(byte));
}

using CodedCell = std::array<std::uint8_t, weather_cell_bytes>;

constexpr float turn = 2.0F * std::numbers::pi_v<float>;

// Each wave system's three bytes: its height, its period, then its heading.
[[nodiscard]] std::array<std::uint8_t, 3> coded(const CellWaves& waves) noexcept {
    const float period = waves.peak > 0.0F ? turn / waves.peak : 0.0F;
    const float angle = std::atan2(waves.heading.y, waves.heading.x) / turn;
    const float round = std::isnan(angle) ? 0.0F : angle - std::floor(angle);
    return {rooted(waves.height, wave_steps),
            period > 0.0F ? std::max(to_byte(period * period_steps), std::uint8_t{1})
                          : std::uint8_t{0},
            static_cast<std::uint8_t>(std::lround(round * heading_steps) % 256)};
}

[[nodiscard]] CellWaves decoded_waves(std::uint8_t height, std::uint8_t period,
                                      std::uint8_t heading) noexcept {
    const float angle = static_cast<float>(heading) / heading_steps * turn;
    return {.height = squared(height, wave_steps),
            .peak = period > 0 ? turn * period_steps / static_cast<float>(period) : 0.0F,
            .heading = {std::cos(angle), std::sin(angle)}};
}

[[nodiscard]] std::uint8_t finer(float value, float steps) noexcept {
    return speed(value * steps);
}

[[nodiscard]] CodedCell coded(const CellWeather& cell) noexcept {
    const auto wind_sea = coded(cell.wind_sea);
    const auto first = coded(cell.swells[0]);
    const auto second = coded(cell.swells[1]);
    return {rooted(cell.cloud, cloud_steps),
            rooted(cell.falling, falling_steps),
            to_byte(cell.snow * snow_steps),
            speed(cell.drift.x),
            speed(cell.drift.y),
            finer(cell.wind.x, wind_steps),
            finer(cell.wind.y, wind_steps),
            to_byte(cell.humidity * humidity_steps),
            to_byte(cell.fog_top / fog_step),
            to_byte(cell.base / height_step),
            to_byte(cell.top / height_step),
            wind_sea[0],
            wind_sea[1],
            wind_sea[2],
            first[0],
            first[1],
            first[2],
            second[0],
            second[1],
            second[2],
            finer(cell.current.x, current_steps),
            finer(cell.current.y, current_steps),
            to_byte(cell.cover * cover_steps)};
}

[[nodiscard]] CellWeather decoded(const CodedCell& bytes) noexcept {
    return {.cloud = squared(bytes[0], cloud_steps),
            .cover = static_cast<float>(bytes[22]) / cover_steps,
            .falling = squared(bytes[1], falling_steps),
            .snow = static_cast<float>(bytes[2]) / snow_steps,
            .drift = {speed_of(bytes[3]), speed_of(bytes[4])},
            .wind = {speed_of(bytes[5]) / wind_steps, speed_of(bytes[6]) / wind_steps},
            .humidity = static_cast<float>(bytes[7]) / humidity_steps,
            .fog_top = static_cast<float>(bytes[8]) * fog_step,
            .base = static_cast<float>(bytes[9]) * height_step,
            .top = static_cast<float>(bytes[10]) * height_step,
            .wind_sea = decoded_waves(bytes[11], bytes[12], bytes[13]),
            .swells = {decoded_waves(bytes[14], bytes[15], bytes[16]),
                       decoded_waves(bytes[17], bytes[18], bytes[19])},
            .current = {speed_of(bytes[20]) / current_steps, speed_of(bytes[21]) / current_steps}};
}

template <typename T>
[[nodiscard]] Result<T> read(ByteReader& in, std::string_view what) {
    auto value = in.read<T>();
    if (!value) {
        return make_error(ErrorCode::Parse, std::format("{} cut short", what));
    }
    return value;
}

// A u16 length, at most `most`, then that many bytes.
[[nodiscard]] Result<std::span<const std::byte>> read_sized(ByteReader& in, std::size_t most,
                                                            std::string_view what) {
    const auto length = read<std::uint16_t>(in, what);
    if (!length) {
        return std::unexpected(length.error());
    }
    if (*length > most) {
        return make_error(
            ErrorCode::OutOfRange,
            std::format("{} of {} bytes is over the {} allowed", what, *length, most));
    }
    auto bytes = in.read_bytes(*length);
    if (!bytes) {
        return make_error(ErrorCode::Parse, std::format("{} cut short", what));
    }
    return bytes;
}

[[nodiscard]] Result<std::uint64_t> read_varint(ByteReader& in, std::string_view what) {
    auto value = in.read_varint();
    if (!value) {
        return make_error(ErrorCode::Parse, std::format("{}: {}", what, value.error().message));
    }
    return value;
}

[[nodiscard]] Result<Millimetres> read_position(ByteReader& in, std::string_view what) {
    Millimetres at{0};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        auto value = in.read_signed();
        if (!value) {
            return make_error(ErrorCode::Parse,
                              std::format("{}'s position: {}", what, value.error().message));
        }
        at[axis] = *value;
    }
    return at;
}

void write_position(ByteWriter& out, Millimetres at) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        out.write_signed(at[axis]);
    }
}

void write_float(ByteWriter& out, float value) {
    out.write(std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] Result<float> read_float(ByteReader& in, std::string_view what) {
    const auto bits = in.read<std::uint32_t>();
    if (!bits) {
        return make_error(ErrorCode::Parse, std::format("{} cut short", what));
    }
    return std::bit_cast<float>(*bits);
}

// A mesh: its origin, its vertices a float at a time, and its indices as varints.
void write_mesh(ByteWriter& out, const SentMesh& mesh, std::size_t most_vertices,
                std::size_t most_indices) {
    HOG_ASSERT(mesh.vertices.size() <= most_vertices, "a mesh with too many vertices to send");
    HOG_ASSERT(mesh.indices.size() <= most_indices, "a mesh with too many indices to send");
    write_position(out, mesh.origin);
    out.write(static_cast<std::uint32_t>(mesh.vertices.size()));
    out.write(static_cast<std::uint32_t>(mesh.indices.size()));
    for (const MeshVertex& vertex : mesh.vertices) {
        for (const float axis : vertex.position) {
            write_float(out, axis);
        }
        for (const float axis : vertex.normal) {
            out.write(static_cast<std::uint16_t>(
                static_cast<std::int16_t>(std::lround(std::clamp(axis, -1.0F, 1.0F) * 32'767.0F))));
        }
        for (const std::uint8_t tile : vertex.tiles) {
            out.write(tile);
        }
        out.write(vertex.corner);
    }
    for (const std::uint32_t index : mesh.indices) {
        out.write_varint(index);
    }
}

[[nodiscard]] Result<SentMesh> read_mesh(ByteReader& in, std::size_t most_vertices,
                                         std::size_t most_indices) {
    SentMesh mesh;
    auto origin = read_position(in, "a mesh");
    const auto vertices = in.read<std::uint32_t>();
    const auto indices = in.read<std::uint32_t>();
    if (!origin || !vertices || !indices) {
        return make_error(ErrorCode::Parse, "a mesh cut short");
    }
    if (*vertices > most_vertices || *indices > most_indices || *indices % 3 != 0) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("a mesh of {} vertices and {} indices", *vertices, *indices));
    }
    mesh.origin = *origin;
    mesh.vertices.reserve(*vertices);
    for (std::uint32_t at = 0; at < *vertices; ++at) {
        MeshVertex vertex;
        for (float& axis : vertex.position) {
            const auto value = read_float(in, "a mesh");
            if (!value) {
                return std::unexpected(value.error());
            }
            axis = *value;
        }
        for (float& axis : vertex.normal) {
            const auto value = in.read<std::uint16_t>();
            if (!value) {
                return make_error(ErrorCode::Parse, "a mesh cut short");
            }
            axis = static_cast<float>(static_cast<std::int16_t>(*value)) / 32'767.0F;
        }
        for (std::uint8_t& tile : vertex.tiles) {
            const auto value = in.read<std::uint8_t>();
            if (!value) {
                return make_error(ErrorCode::Parse, "a mesh cut short");
            }
            tile = *value;
        }
        const auto corner = in.read<std::uint8_t>();
        if (!corner) {
            return make_error(ErrorCode::Parse, "a mesh cut short");
        }
        vertex.corner = *corner;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices.reserve(*indices);
    for (std::uint32_t at = 0; at < *indices; ++at) {
        const auto index = in.read_varint();
        if (!index) {
            return make_error(ErrorCode::Parse, "a mesh cut short");
        }
        if (*index >= *vertices) {
            return make_error(ErrorCode::OutOfRange, "a mesh index past its vertices");
        }
        mesh.indices.push_back(static_cast<std::uint32_t>(*index));
    }
    return mesh;
}

// A stance and facing, as a move or a placing ends with.
void write_stance(ByteWriter& out, Stance stance, Facing facing) {
    out.write(static_cast<std::uint8_t>(stance));
    out.write(facing.heading);
    out.write(static_cast<std::uint16_t>(facing.pitch));
}

struct StanceAndFacing {
    Stance stance = Stance::Walking;
    Facing facing{};
};

[[nodiscard]] Result<StanceAndFacing> read_stance(ByteReader& in, std::string_view what) {
    const auto stance = read<std::uint8_t>(in, what);
    const auto heading = read<std::uint16_t>(in, what);
    const auto pitch = read<std::uint16_t>(in, what);
    if (!stance || !heading || !pitch) {
        return make_error(ErrorCode::Parse, std::format("{} cut short", what));
    }
    if (*stance > static_cast<std::uint8_t>(Stance::Flying)) {
        return make_error(ErrorCode::Unsupported, std::format("{} of stance {}", what, *stance));
    }
    return StanceAndFacing{
        .stance = static_cast<Stance>(*stance),
        .facing = {.heading = *heading, .pitch = static_cast<std::int16_t>(*pitch)}};
}

// A list's length, which can't be more than the bytes left, as each entry takes one at least.
[[nodiscard]] Result<std::size_t> read_count(ByteReader& in, std::string_view what) {
    const auto count = read_varint(in, what);
    if (!count) {
        return std::unexpected(count.error());
    }
    if (*count > in.remaining()) {
        return make_error(ErrorCode::Parse, std::format("{} of {} cut short", what, *count));
    }
    return static_cast<std::size_t>(*count);
}

// Ids in a list go up, each written as the gap from the one before, the first from 0.
class IdReader {
public:
    explicit IdReader(std::string_view what) noexcept : what_(what) {}

    [[nodiscard]] Result<NetId> next(ByteReader& in) {
        const auto gap = read_varint(in, what_);
        if (!gap) {
            return std::unexpected(gap.error());
        }
        if (*gap == 0 || *gap > std::numeric_limits<std::uint32_t>::max() - last_) {
            return make_error(ErrorCode::Parse, std::format("{} out of order", what_));
        }
        last_ += static_cast<std::uint32_t>(*gap);
        return NetId{last_};
    }

private:
    std::string_view what_;
    std::uint32_t last_ = 0;
};

class IdWriter {
public:
    void next(ByteWriter& out, NetId id) {
        const auto value = std::to_underlying(id);
        HOG_ASSERT(value > last_, "a snapshot's ids must go up");
        out.write_varint(value - last_);
        last_ = value;
    }

private:
    std::uint32_t last_ = 0;
};

void write_sized(ByteWriter& out, std::span<const std::byte> bytes) {
    out.write(static_cast<std::uint16_t>(bytes.size()));
    out.write_bytes(bytes);
}

// Chat is shown to other players, so it carries no control characters to trouble a terminal.
[[nodiscard]] bool printable(std::span<const std::byte> text) noexcept {
    return std::ranges::none_of(text, [](std::byte byte) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        return value < 0x20U || value == 0x7FU;
    });
}

// A time scale in game seconds a real one, which is never nothing.
[[nodiscard]] Result<std::uint32_t> read_scale(ByteReader& in, std::string_view what) {
    const auto scale = read<std::uint32_t>(in, what);
    if (!scale) {
        return std::unexpected(scale.error());
    }
    if (*scale == 0) {
        return make_error(ErrorCode::OutOfRange, std::format("{} to no time at all", what));
    }
    return scale;
}

[[nodiscard]] Result<Message> finished(const ByteReader& in, Message message) {
    if (in.remaining() != 0) {
        return make_error(ErrorCode::Parse,
                          std::format("{} bytes left over after the message", in.remaining()));
    }
    return message;
}

[[nodiscard]] Result<Message> read_hello(ByteReader& in) {
    Hello hello;
    const auto spoken = read<std::uint32_t>(in, "hello");
    if (!spoken) {
        return std::unexpected(spoken.error());
    }
    hello.version = *spoken;
    if (hello.version != protocol::version) {
        return Message{std::move(hello)};
    }
    const auto token = read_sized(in, most_token, "connect token");
    if (!token) {
        return std::unexpected(token.error());
    }
    hello.token.assign(token->begin(), token->end());
    return finished(in, std::move(hello));
}

[[nodiscard]] Result<Message> read_refused(ByteReader& in) {
    Refused refused;
    const auto spoken = read<std::uint32_t>(in, "refusal");
    const auto reason = read<std::uint8_t>(in, "refusal");
    if (!spoken || !reason) {
        return make_error(ErrorCode::Parse, "refusal cut short");
    }
    if (*reason > static_cast<std::uint8_t>(Refusal::Silent)) {
        return make_error(ErrorCode::Parse, std::format("unknown refusal {}", *reason));
    }
    refused.version = *spoken;
    refused.reason = static_cast<Refusal>(*reason);
    const auto detail = read_sized(in, most_detail, "refusal's detail");
    if (!detail) {
        return std::unexpected(detail.error());
    }
    refused.detail.reserve(detail->size());
    for (const std::byte byte : *detail) {
        refused.detail.push_back(static_cast<char>(byte));
    }
    return finished(in, std::move(refused));
}

// A list in a snapshot: its length, then each entry's id and what read_one reads after it.
template <typename Read>
[[nodiscard]] auto read_list(ByteReader& in, std::string_view what, Read read_one)
    -> Result<std::vector<typename std::invoke_result_t<Read&, ByteReader&, NetId>::value_type>> {
    using Entry = std::invoke_result_t<Read&, ByteReader&, NetId>::value_type;
    const auto count = read_count(in, what);
    if (!count) {
        return std::unexpected(count.error());
    }
    std::vector<Entry> entries;
    IdReader ids{what};
    for (std::size_t index = 0; index < *count; ++index) {
        const auto id = ids.next(in);
        if (!id) {
            return std::unexpected(id.error());
        }
        auto entry = read_one(in, *id);
        if (!entry) {
            return std::unexpected(entry.error());
        }
        entries.push_back(std::move(*entry));
    }
    return entries;
}

[[nodiscard]] Result<Entered> read_entered(ByteReader& in, NetId id) {
    const auto at = read_position(in, "an entity entering");
    if (!at) {
        return std::unexpected(at.error());
    }
    const auto length = read<std::uint8_t>(in, "an entity's name");
    if (!length) {
        return std::unexpected(length.error());
    }
    if (*length > most_name) {
        return make_error(
            ErrorCode::OutOfRange,
            std::format("a name of {} bytes is over the {} allowed", *length, most_name));
    }
    const auto name = in.read_bytes(*length);
    if (!name) {
        return make_error(ErrorCode::Parse, "an entity's name cut short");
    }
    Entered entity{.id = id, .at = *at, .name = {}};
    for (const std::byte byte : *name) {
        entity.name.push_back(static_cast<char>(byte));
    }
    return entity;
}

[[nodiscard]] Result<Changed> read_changed(ByteReader& in, NetId id) {
    const auto what = read<std::uint8_t>(in, "an entity's changes");
    if (!what) {
        return std::unexpected(what.error());
    }
    if ((*what & ~known_changes) != 0) {
        return make_error(ErrorCode::Parse, std::format("unknown changes {:#x}", *what));
    }
    Changed entity{.id = id, .moved_by = {}};
    if ((*what & changed_position) != 0) {
        const auto moved_by = read_position(in, "an entity changed");
        if (!moved_by) {
            return std::unexpected(moved_by.error());
        }
        entity.moved_by = *moved_by;
    }
    return entity;
}

[[nodiscard]] Result<Message> read_snapshot(ByteReader& in) {
    Snapshot snapshot;
    const auto tick = read<std::uint32_t>(in, "snapshot");
    if (!tick) {
        return std::unexpected(tick.error());
    }
    const auto age = read_varint(in, "snapshot's baseline");
    if (!age) {
        return std::unexpected(age.error());
    }
    snapshot.tick = *tick;
    if (*age > *tick) {
        return make_error(ErrorCode::Parse, "snapshot's baseline before the first tick");
    }
    if (*age != 0) {
        snapshot.baseline = static_cast<std::uint32_t>(*tick - *age);
    }
    auto entered = read_list(in, "entities entering", read_entered);
    if (!entered) {
        return std::unexpected(entered.error());
    }
    auto changed = read_list(in, "entities changed", read_changed);
    if (!changed) {
        return std::unexpected(changed.error());
    }
    auto left = read_list(in, "entities leaving",
                          [](ByteReader& /*in*/, NetId id) { return Result<NetId>{id}; });
    if (!left) {
        return std::unexpected(left.error());
    }
    snapshot.entered = std::move(*entered);
    snapshot.changed = std::move(*changed);
    snapshot.left = std::move(*left);
    return finished(in, std::move(snapshot));
}

void write_snapshot(ByteWriter& out, const Snapshot& snapshot) {
    out.write(static_cast<std::uint16_t>(Kind::Snapshot));
    out.write(snapshot.tick);
    HOG_ASSERT(!snapshot.baseline || *snapshot.baseline < snapshot.tick,
               "a snapshot's baseline must come before it");
    out.write_varint(snapshot.baseline ? snapshot.tick - *snapshot.baseline : 0);

    out.write_varint(snapshot.entered.size());
    IdWriter entering;
    for (const Entered& entity : snapshot.entered) {
        HOG_ASSERT(entity.name.size() <= most_name, "an entity's name too long to send");
        entering.next(out, entity.id);
        write_position(out, entity.at);
        out.write(static_cast<std::uint8_t>(entity.name.size()));
        out.write_bytes(std::as_bytes(std::span{entity.name}));
    }

    out.write_varint(snapshot.changed.size());
    IdWriter changing;
    for (const Changed& entity : snapshot.changed) {
        changing.next(out, entity.id);
        out.write(entity.moved_by ? changed_position : std::uint8_t{0});
        if (entity.moved_by) {
            write_position(out, *entity.moved_by);
        }
    }

    out.write_varint(snapshot.left.size());
    IdWriter leaving;
    for (const NetId id : snapshot.left) {
        leaving.next(out, id);
    }
}

// A ball to dig or build: its centre and a radius of some width, at most most_edit.
struct Ball {
    Millimetres centre;
    std::uint32_t radius;
};

[[nodiscard]] Result<Ball> read_ball(ByteReader& in, std::string_view what) {
    const auto centre = read_position(in, what);
    if (!centre) {
        return std::unexpected(centre.error());
    }
    const auto radius = read<std::uint32_t>(in, what);
    if (!radius) {
        return std::unexpected(radius.error());
    }
    if (*radius == 0 || *radius > most_edit) {
        return make_error(ErrorCode::OutOfRange, std::format("{} {} mm across; 1 to {} allowed",
                                                             what, *radius, most_edit));
    }
    return Ball{.centre = *centre, .radius = *radius};
}

[[nodiscard]] Result<Message> read_dug(ByteReader& in) {
    const auto ball = read_ball(in, "a digging");
    if (!ball) {
        return std::unexpected(ball.error());
    }
    return finished(in, Dug{.centre = ball->centre, .radius = ball->radius});
}

[[nodiscard]] Result<Message> read_built(ByteReader& in) {
    const auto ball = read_ball(in, "a building");
    if (!ball) {
        return std::unexpected(ball.error());
    }
    const auto material = read<std::uint8_t>(in, "a building");
    if (!material) {
        return std::unexpected(material.error());
    }
    return finished(in,
                    Built{.centre = ball->centre, .radius = ball->radius, .material = *material});
}

[[nodiscard]] Result<Message> read_columns(ByteReader& in) {
    const auto count = read<std::uint16_t>(in, "tile columns");
    if (!count) {
        return std::unexpected(count.error());
    }
    if (*count > most_columns) {
        return make_error(
            ErrorCode::OutOfRange,
            std::format("{} tile columns in a message; {} allowed", *count, most_columns));
    }
    Columns columns;
    columns.columns.reserve(*count);
    for (std::uint16_t k = 0; k < *count; ++k) {
        const auto face = read<std::uint8_t>(in, "a tile column");
        const auto i = read<std::uint32_t>(in, "a tile column");
        const auto j = read<std::uint32_t>(in, "a tile column");
        const auto level = read<std::uint8_t>(in, "a tile column");
        const auto length = read<std::uint32_t>(in, "a tile column");
        if (!face || !i || !j || !level || !length) {
            return make_error(ErrorCode::Parse, "a tile column cut short");
        }
        if (*length > most_column) {
            return make_error(ErrorCode::OutOfRange,
                              std::format("a tile column of {} bytes is over the {} allowed",
                                          *length, most_column));
        }
        const auto bytes = in.read_bytes(*length);
        if (!bytes) {
            return make_error(ErrorCode::Parse, "a tile column cut short");
        }
        columns.columns.push_back({.face = *face,
                                   .i = *i,
                                   .j = *j,
                                   .level = *level,
                                   .bytes = {bytes->begin(), bytes->end()}});
    }
    return finished(in, std::move(columns));
}

[[nodiscard]] Result<Message> read_chunk(ByteReader& in) {
    const auto key = in.read<std::uint64_t>();
    const auto version = in.read<std::uint32_t>();
    const auto part = in.read<std::uint8_t>();
    const auto parts = in.read<std::uint8_t>();
    if (!key || !version || !part || !parts) {
        return make_error(ErrorCode::Parse, "a chunk cut short");
    }
    if (*parts == 0 || *part >= *parts) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("part {} of {} of a chunk", *part, *parts));
    }
    auto ground = read_mesh(in, most_ground_vertices, most_ground_indices);
    if (!ground) {
        return std::unexpected(ground.error());
    }
    auto water = read_mesh(in, most_water_vertices, most_water_indices);
    if (!water) {
        return std::unexpected(water.error());
    }
    return finished(in, Chunk{.key = *key,
                              .version = *version,
                              .part = *part,
                              .parts = *parts,
                              .ground = std::move(*ground),
                              .water = std::move(*water)});
}

[[nodiscard]] Result<Message> read_chunk_plan(ByteReader& in) {
    const auto count = in.read<std::uint16_t>();
    if (!count) {
        return make_error(ErrorCode::Parse, "a chunk plan cut short");
    }
    if (*count > most_planned) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("{} chunks in a plan; {} allowed", *count, most_planned));
    }
    ChunkPlan plan;
    plan.wanted.reserve(*count);
    for (std::uint16_t at = 0; at < *count; ++at) {
        const auto key = in.read<std::uint64_t>();
        const auto version = in.read<std::uint32_t>();
        if (!key || !version) {
            return make_error(ErrorCode::Parse, "a chunk plan cut short");
        }
        plan.wanted.push_back({.key = *key, .version = *version});
    }
    return finished(in, std::move(plan));
}

[[nodiscard]] Result<Message> read_chunks_asked(ByteReader& in) {
    const auto count = in.read<std::uint16_t>();
    if (!count) {
        return make_error(ErrorCode::Parse, "chunks asked for cut short");
    }
    if (*count > most_asked) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("{} chunks asked for; {} allowed", *count, most_asked));
    }
    ChunksAsked asked;
    for (std::uint16_t at = 0; at < *count; ++at) {
        const auto key = in.read<std::uint64_t>();
        if (!key) {
            return make_error(ErrorCode::Parse, "chunks asked for cut short");
        }
        asked.keys.push_back(*key);
    }
    return finished(in, std::move(asked));
}

[[nodiscard]] Result<Message> read_grains(ByteReader& in) {
    const auto tick = read<std::uint32_t>(in, "grains");
    const auto count = read<std::uint16_t>(in, "grains");
    if (!tick || !count) {
        return make_error(ErrorCode::Parse, "grains cut short");
    }
    if (*count > most_grains) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("{} grains in a message; {} allowed", *count, most_grains));
    }
    Grains grains{.tick = *tick, .flying = {}};
    grains.flying.reserve(*count);
    std::uint32_t last = 0;
    for (std::uint16_t k = 0; k < *count; ++k) {
        const auto id = read<std::uint32_t>(in, "a grain");
        const auto at = read_position(in, "a grain");
        if (!id || !at) {
            return make_error(ErrorCode::Parse, "a grain cut short");
        }
        if (*id == 0 || (k > 0 && *id <= last)) {
            return make_error(ErrorCode::Parse, "grains out of order");
        }
        last = *id;
        Flying one{.id = NetId{*id}, .at = *at};
        for (std::int16_t& axis : one.velocity) {
            const auto value = read<std::uint16_t>(in, "a grain");
            if (!value) {
                return std::unexpected(value.error());
            }
            axis = static_cast<std::int16_t>(*value);
        }
        const auto material = read<std::uint8_t>(in, "a grain");
        const auto amount = read<std::uint32_t>(in, "a grain");
        if (!material || !amount) {
            return make_error(ErrorCode::Parse, "a grain cut short");
        }
        one.material = *material;
        one.amount = *amount;
        grains.flying.push_back(one);
    }
    return finished(in, std::move(grains));
}

[[nodiscard]] Result<Message> read_said(ByteReader& in) {
    const auto from = read_sized(in, most_name, "chat's speaker");
    if (!from) {
        return std::unexpected(from.error());
    }
    const auto text = read_sized(in, most_said, "a line of chat");
    if (!text) {
        return std::unexpected(text.error());
    }
    if (!printable(*from) || !printable(*text)) {
        return make_error(ErrorCode::Parse, "chat with control characters");
    }
    Said said;
    said.from.reserve(from->size());
    for (const std::byte byte : *from) {
        said.from.push_back(static_cast<char>(byte));
    }
    said.text.reserve(text->size());
    for (const std::byte byte : *text) {
        said.text.push_back(static_cast<char>(byte));
    }
    return finished(in, std::move(said));
}

[[nodiscard]] Result<Message> read_weather(ByteReader& in) {
    const auto level = read<std::uint8_t>(in, "weather");
    const auto fade = read<std::uint32_t>(in, "weather");
    const auto pace = read<std::uint32_t>(in, "weather");
    if (!level || !fade || !pace) {
        return make_error(ErrorCode::Parse, "weather cut short");
    }
    if (*level > most_weather_level) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("weather at level {} is finer than the {} allowed", *level,
                                      most_weather_level));
    }
    Weather weather{.level = *level,
                    .fade = std::chrono::milliseconds{*fade},
                    .pace = static_cast<float>(*pace) / pace_steps,
                    .cells = {}};
    const std::size_t count = weather_cells(*level);
    const auto bytes = in.read_bytes(count * weather_cell_bytes);
    if (!bytes) {
        return make_error(ErrorCode::Parse, "weather's cells cut short");
    }
    weather.cells.reserve(count);
    for (std::size_t first = 0; first < bytes->size(); first += weather_cell_bytes) {
        CodedCell cell{};
        std::ranges::transform(bytes->subspan(first, weather_cell_bytes), cell.begin(),
                               [](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
        weather.cells.push_back(decoded(cell));
    }
    return finished(in, std::move(weather));
}

void write_weather(ByteWriter& out, const Weather& weather) {
    HOG_ASSERT(weather.level <= most_weather_level, "weather too fine to send");
    HOG_ASSERT(weather.cells.size() == weather_cells(weather.level),
               "weather needs a cell for each at its level");
    out.write(static_cast<std::uint16_t>(Kind::Weather));
    out.write(weather.level);
    out.write(static_cast<std::uint32_t>(std::clamp<std::chrono::milliseconds::rep>(
        weather.fade.count(), 0, std::numeric_limits<std::uint32_t>::max())));
    out.write(
        static_cast<std::uint32_t>(std::clamp(std::lround(weather.pace * pace_steps), 0L,
                                              long{std::numeric_limits<std::int32_t>::max()})));
    for (const CellWeather& cell : weather.cells) {
        for (const std::uint8_t byte : coded(cell)) {
            out.write(byte);
        }
    }
}

} // namespace

CellWeather as_sent(const CellWeather& cell) noexcept {
    return decoded(coded(cell));
}

Millimetres to_millimetres(WorldPosition position) noexcept {
    const DVec3 metres = position.metres();
    return {std::llround(metres.x * 1'000.0), std::llround(metres.y * 1'000.0),
            std::llround(metres.z * 1'000.0)};
}

WorldPosition to_world(Millimetres millimetres) noexcept {
    return WorldPosition{DVec3{millimetres} / 1'000.0};
}

std::vector<std::byte> encode(const Message& message) {
    ByteWriter out;
    std::visit(
        [&](const auto& body) {
            using Body = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<Body, Hello>) {
                HOG_ASSERT(body.token.size() <= most_token, "connect token too long to send");
                out.write(static_cast<std::uint16_t>(Kind::Hello));
                out.write(body.version);
                write_sized(out, body.token);
            } else if constexpr (std::is_same_v<Body, Welcome>) {
                HOG_ASSERT(body.planet.size() <= most_planet, "a planet too long to send");
                HOG_ASSERT(body.ground.size() <= most_planet, "voxel layers too long to send");
                out.write(static_cast<std::uint16_t>(Kind::Welcome));
                out.write(body.version);
                out.write(body.world);
                write_sized(out, body.planet);
                write_sized(out, body.ground);
            } else if constexpr (std::is_same_v<Body, Moved>) {
                out.write(static_cast<std::uint16_t>(Kind::Moved));
                out.write(body.sequence);
                out.write(body.corrections);
                write_position(out, body.at);
                write_stance(out, body.stance, body.facing);
            } else if constexpr (std::is_same_v<Body, Placed>) {
                out.write(static_cast<std::uint16_t>(Kind::Placed));
                write_position(out, body.at);
                write_stance(out, body.stance, body.facing);
            } else if constexpr (std::is_same_v<Body, Dug>) {
                out.write(static_cast<std::uint16_t>(Kind::Dug));
                write_position(out, body.centre);
                out.write(body.radius);
            } else if constexpr (std::is_same_v<Body, Built>) {
                out.write(static_cast<std::uint16_t>(Kind::Built));
                write_position(out, body.centre);
                out.write(body.radius);
                out.write(body.material);
            } else if constexpr (std::is_same_v<Body, Columns>) {
                HOG_ASSERT(body.columns.size() <= most_columns,
                           "too many tile columns to send at once");
                out.write(static_cast<std::uint16_t>(Kind::Columns));
                out.write(static_cast<std::uint16_t>(body.columns.size()));
                for (const SentColumn& column : body.columns) {
                    HOG_ASSERT(column.bytes.size() <= most_column, "a tile column too big to send");
                    out.write(column.face);
                    out.write(column.i);
                    out.write(column.j);
                    out.write(column.level);
                    out.write(static_cast<std::uint32_t>(column.bytes.size()));
                    out.write_bytes(column.bytes);
                }
            } else if constexpr (std::is_same_v<Body, ChunkPlan>) {
                HOG_ASSERT(body.wanted.size() <= most_planned, "too many chunks planned to send");
                out.write(static_cast<std::uint16_t>(Kind::ChunkPlan));
                out.write(static_cast<std::uint16_t>(body.wanted.size()));
                for (const PlannedChunk& planned : body.wanted) {
                    out.write(planned.key);
                    out.write(planned.version);
                }
            } else if constexpr (std::is_same_v<Body, ViewDetail>) {
                out.write(static_cast<std::uint16_t>(Kind::ViewDetail));
                write_float(out, body.ratio);
                write_float(out, body.near);
                write_float(out, body.slope);
            } else if constexpr (std::is_same_v<Body, ChunksAsked>) {
                HOG_ASSERT(body.keys.size() <= most_asked, "too many chunks asked for at once");
                out.write(static_cast<std::uint16_t>(Kind::ChunksAsked));
                out.write(static_cast<std::uint16_t>(body.keys.size()));
                for (const std::uint64_t key : body.keys) {
                    out.write(key);
                }
            } else if constexpr (std::is_same_v<Body, Ground>) {
                out.write(static_cast<std::uint16_t>(Kind::Ground));
                write_float(out, body.height);
            } else if constexpr (std::is_same_v<Body, Chunk>) {
                out.write(static_cast<std::uint16_t>(Kind::Chunk));
                out.write(body.key);
                out.write(body.version);
                out.write(body.part);
                out.write(body.parts);
                write_mesh(out, body.ground, most_ground_vertices, most_ground_indices);
                write_mesh(out, body.water, most_water_vertices, most_water_indices);
            } else if constexpr (std::is_same_v<Body, Grains>) {
                HOG_ASSERT(body.flying.size() <= most_grains, "too much grain to send at once");
                out.write(static_cast<std::uint16_t>(Kind::Grains));
                out.write(body.tick);
                out.write(static_cast<std::uint16_t>(body.flying.size()));
                for (const Flying& one : body.flying) {
                    out.write(std::to_underlying(one.id));
                    write_position(out, one.at);
                    for (const std::int16_t axis : one.velocity) {
                        out.write(static_cast<std::uint16_t>(axis));
                    }
                    out.write(one.material);
                    out.write(one.amount);
                }
            } else if constexpr (std::is_same_v<Body, Ack>) {
                out.write(static_cast<std::uint16_t>(Kind::Ack));
                out.write(body.tick);
            } else if constexpr (std::is_same_v<Body, Snapshot>) {
                write_snapshot(out, body);
            } else if constexpr (std::is_same_v<Body, Weather>) {
                write_weather(out, body);
            } else if constexpr (std::is_same_v<Body, Corrected>) {
                out.write(static_cast<std::uint16_t>(Kind::Corrected));
                out.write(body.count);
                write_position(out, body.at);
            } else if constexpr (std::is_same_v<Body, Hurry>) {
                out.write(static_cast<std::uint16_t>(Kind::Hurry));
                out.write(body.scale);
            } else if constexpr (std::is_same_v<Body, Clock>) {
                out.write(static_cast<std::uint16_t>(Kind::Clock));
                out.write(body.scale);
            } else if constexpr (std::is_same_v<Body, Said>) {
                out.write(static_cast<std::uint16_t>(Kind::Said));
                const std::string_view from{body.from};
                write_sized(out, std::as_bytes(
                                     std::span{from.substr(0, std::min(from.size(), most_name))}));
                const std::string_view text{body.text};
                write_sized(out, std::as_bytes(
                                     std::span{text.substr(0, std::min(text.size(), most_said))}));
            } else {
                static_assert(std::is_same_v<Body, Refused>);
                out.write(static_cast<std::uint16_t>(Kind::Refused));
                out.write(body.version);
                out.write(static_cast<std::uint8_t>(body.reason));
                const std::string_view detail{body.detail};
                write_sized(out, std::as_bytes(std::span{
                                     detail.substr(0, std::min(detail.size(), most_detail))}));
            }
        },
        message);
    return std::move(out).take();
}

Result<Message> decode(std::span<const std::byte> bytes) {
    ByteReader in{bytes};
    const auto kind = read<std::uint16_t>(in, "message");
    if (!kind) {
        return std::unexpected(kind.error());
    }
    switch (static_cast<Kind>(*kind)) {
    case Kind::Hello:
        return read_hello(in);
    case Kind::Welcome: {
        const auto spoken = read<std::uint32_t>(in, "welcome");
        if (!spoken) {
            return std::unexpected(spoken.error());
        }
        const auto world = read<std::uint64_t>(in, "welcome");
        if (!world) {
            return std::unexpected(world.error());
        }
        const auto planet = read_sized(in, most_planet, "a welcome");
        if (!planet) {
            return std::unexpected(planet.error());
        }
        const auto ground = read_sized(in, most_planet, "a welcome");
        if (!ground) {
            return std::unexpected(ground.error());
        }
        return finished(in, Welcome{.version = *spoken,
                                    .world = *world,
                                    .planet = {planet->begin(), planet->end()},
                                    .ground = {ground->begin(), ground->end()}});
    }
    case Kind::Refused:
        return read_refused(in);
    case Kind::Moved: {
        const auto sequence = read<std::uint32_t>(in, "a move");
        const auto corrections = read<std::uint32_t>(in, "a move");
        const auto at = read_position(in, "a move");
        if (!sequence || !corrections || !at) {
            return make_error(ErrorCode::Parse, "a move cut short");
        }
        const auto stance = read_stance(in, "a move");
        if (!stance) {
            return std::unexpected(stance.error());
        }
        return finished(in, Moved{.sequence = *sequence,
                                  .corrections = *corrections,
                                  .at = *at,
                                  .stance = stance->stance,
                                  .facing = stance->facing});
    }
    case Kind::Placed: {
        const auto at = read_position(in, "a placing");
        if (!at) {
            return std::unexpected(at.error());
        }
        const auto stance = read_stance(in, "a placing");
        if (!stance) {
            return std::unexpected(stance.error());
        }
        return finished(in, Placed{.at = *at, .stance = stance->stance, .facing = stance->facing});
    }
    case Kind::Corrected: {
        const auto count = read<std::uint32_t>(in, "a correction");
        const auto at = read_position(in, "a correction");
        if (!count || !at) {
            return make_error(ErrorCode::Parse, "a correction cut short");
        }
        return finished(in, Corrected{.count = *count, .at = *at});
    }
    case Kind::Ack: {
        const auto tick = read<std::uint32_t>(in, "an ack");
        if (!tick) {
            return std::unexpected(tick.error());
        }
        return finished(in, Ack{.tick = *tick});
    }
    case Kind::Snapshot:
        return read_snapshot(in);
    case Kind::Weather:
        return read_weather(in);
    case Kind::Dug:
        return read_dug(in);
    case Kind::Built:
        return read_built(in);
    case Kind::Columns:
        return read_columns(in);
    case Kind::Grains:
        return read_grains(in);
    case Kind::ViewDetail: {
        const auto ratio = read_float(in, "a view detail");
        const auto near = read_float(in, "a view detail");
        const auto slope = read_float(in, "a view detail");
        if (!ratio || !near || !slope) {
            return make_error(ErrorCode::Parse, "a view detail cut short");
        }
        // Within what's allowed, and a number.
        if (!(*ratio >= least_detail_ratio && *ratio <= most_detail_ratio &&
              *near >= least_detail_near && *near <= most_detail_near &&
              *slope >= least_detail_slope && *slope <= most_detail_slope)) {
            return make_error(
                ErrorCode::OutOfRange,
                std::format("a view detail of ratio {} near {} slope {}", *ratio, *near, *slope));
        }
        return finished(in, ViewDetail{.ratio = *ratio, .near = *near, .slope = *slope});
    }
    case Kind::ChunkPlan:
        return read_chunk_plan(in);
    case Kind::ChunksAsked:
        return read_chunks_asked(in);
    case Kind::Ground: {
        const auto height = read_float(in, "ground");
        if (!height) {
            return std::unexpected(height.error());
        }
        return finished(in, Ground{.height = *height});
    }
    case Kind::Chunk:
        return read_chunk(in);
    case Kind::Said:
        return read_said(in);
    case Kind::Hurry: {
        const auto scale = read_scale(in, "a hurry");
        if (!scale) {
            return std::unexpected(scale.error());
        }
        return finished(in, Hurry{.scale = *scale});
    }
    case Kind::Clock: {
        const auto scale = read_scale(in, "a clock");
        if (!scale) {
            return std::unexpected(scale.error());
        }
        return finished(in, Clock{.scale = *scale});
    }
    }
    return make_error(ErrorCode::Unsupported, std::format("unknown message kind {}", *kind));
}

std::string explain(const Refused& refused, std::uint32_t own) {
    switch (refused.reason) {
    case Refusal::Version:
        if (refused.version > own) {
            return std::format("the server speaks protocol {} and this client {}: update the "
                               "client",
                               refused.version, own);
        }
        return std::format("the server speaks protocol {} and this client {}: the server is "
                           "older than this client",
                           refused.version, own);
    case Refusal::Token:
        return std::format("the server didn't accept the login: {}", refused.detail);
    case Refusal::Unexpected:
    case Refusal::Silent:
        break;
    }
    return std::format("the server closed the connection: {}", refused.detail);
}

} // namespace hog::protocol
