#include <hog/core/assert.hpp>
#include <hog/core/bytes.hpp>
#include <hog/protocol/messages.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
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
    Woodlands = 12,
}; // NOLINT(performance-enum-size)

// Which of an entity's changes follow its id in a snapshot.
constexpr std::uint8_t changed_position = 1U;
constexpr std::uint8_t known_changes = changed_position;

// Cloud and falling are sent as square roots, so thin cloud and drizzle keep their detail.
constexpr float cloud_steps = 64.0F;   // a byte's steps to the root of a millimetre
constexpr float falling_steps = 16.0F; // to the root of a millimetre an hour
constexpr float snow_steps = 255.0F;
constexpr float fastest_drift = 127.0F;
constexpr float humidity_steps = 255.0F;
constexpr float fog_step = 32.0F; // metres
constexpr float height_step = 64.0F;
constexpr std::size_t weather_cell_bytes = 9;

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

constexpr std::size_t woodland_cell_bytes = 5;
constexpr float share_steps = 255.0F;

using CodedWoodland = std::array<std::uint8_t, woodland_cell_bytes>;

[[nodiscard]] CodedWoodland coded(const CellWoodland& cell) noexcept {
    return {to_byte(cell.living * share_steps), to_byte(cell.established * share_steps),
            to_byte(cell.failing * share_steps), to_byte(cell.dead * share_steps),
            to_byte(cell.charred * share_steps)};
}

[[nodiscard]] CellWoodland decoded(const CodedWoodland& bytes) noexcept {
    return {.living = static_cast<float>(bytes[0]) / share_steps,
            .established = static_cast<float>(bytes[1]) / share_steps,
            .failing = static_cast<float>(bytes[2]) / share_steps,
            .dead = static_cast<float>(bytes[3]) / share_steps,
            .charred = static_cast<float>(bytes[4]) / share_steps};
}

using CodedCell = std::array<std::uint8_t, weather_cell_bytes>;

[[nodiscard]] CodedCell coded(const CellWeather& cell) noexcept {
    return {rooted(cell.cloud, cloud_steps),
            rooted(cell.falling, falling_steps),
            to_byte(cell.snow * snow_steps),
            speed(cell.drift.x),
            speed(cell.drift.y),
            to_byte(cell.humidity * humidity_steps),
            to_byte(cell.fog_top / fog_step),
            to_byte(cell.base / height_step),
            to_byte(cell.top / height_step)};
}

[[nodiscard]] CellWeather decoded(const CodedCell& bytes) noexcept {
    return {.cloud = squared(bytes[0], cloud_steps),
            .falling = squared(bytes[1], falling_steps),
            .snow = static_cast<float>(bytes[2]) / snow_steps,
            .drift = {speed_of(bytes[3]), speed_of(bytes[4])},
            .humidity = static_cast<float>(bytes[5]) / humidity_steps,
            .fog_top = static_cast<float>(bytes[6]) * fog_step,
            .base = static_cast<float>(bytes[7]) * height_step,
            .top = static_cast<float>(bytes[8]) * height_step};
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
    if (!level || !fade) {
        return make_error(ErrorCode::Parse, "weather cut short");
    }
    if (*level > most_weather_level) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("weather at level {} is finer than the {} allowed", *level,
                                      most_weather_level));
    }
    Weather weather{.level = *level, .fade = std::chrono::milliseconds{*fade}, .cells = {}};
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
    for (const CellWeather& cell : weather.cells) {
        for (const std::uint8_t byte : coded(cell)) {
            out.write(byte);
        }
    }
}

[[nodiscard]] Result<Message> read_woodlands(ByteReader& in) {
    const auto level = read<std::uint8_t>(in, "woodlands");
    if (!level) {
        return make_error(ErrorCode::Parse, "woodlands cut short");
    }
    if (*level > most_weather_level) {
        return make_error(ErrorCode::OutOfRange,
                          std::format("woodlands at level {} are finer than the {} allowed", *level,
                                      most_weather_level));
    }
    const std::size_t count = weather_cells(*level);
    const auto bytes = in.read_bytes(count * woodland_cell_bytes);
    if (!bytes) {
        return make_error(ErrorCode::Parse, "woodlands' cells cut short");
    }
    Woodlands woodlands{.level = *level, .cells = {}};
    woodlands.cells.reserve(count);
    for (std::size_t first = 0; first < bytes->size(); first += woodland_cell_bytes) {
        CodedWoodland cell{};
        std::ranges::transform(bytes->subspan(first, woodland_cell_bytes), cell.begin(),
                               [](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
        woodlands.cells.push_back(decoded(cell));
    }
    return finished(in, std::move(woodlands));
}

void write_woodlands(ByteWriter& out, const Woodlands& woodlands) {
    HOG_ASSERT(woodlands.level <= most_weather_level, "woodlands too fine to send");
    HOG_ASSERT(woodlands.cells.size() == weather_cells(woodlands.level),
               "woodlands need a cell for each at their level");
    out.write(static_cast<std::uint16_t>(Kind::Woodlands));
    out.write(woodlands.level);
    for (const CellWoodland& cell : woodlands.cells) {
        for (const std::uint8_t byte : coded(cell)) {
            out.write(byte);
        }
    }
}

} // namespace

CellWeather as_sent(const CellWeather& cell) noexcept {
    return decoded(coded(cell));
}

CellWoodland as_sent(const CellWoodland& cell) noexcept {
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
                out.write(static_cast<std::uint16_t>(Kind::Welcome));
                out.write(body.version);
            } else if constexpr (std::is_same_v<Body, Moved>) {
                out.write(static_cast<std::uint16_t>(Kind::Moved));
                out.write(body.sequence);
                out.write(body.corrections);
                write_position(out, body.at);
            } else if constexpr (std::is_same_v<Body, Ack>) {
                out.write(static_cast<std::uint16_t>(Kind::Ack));
                out.write(body.tick);
            } else if constexpr (std::is_same_v<Body, Snapshot>) {
                write_snapshot(out, body);
            } else if constexpr (std::is_same_v<Body, Weather>) {
                write_weather(out, body);
            } else if constexpr (std::is_same_v<Body, Woodlands>) {
                write_woodlands(out, body);
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
        return finished(in, Welcome{.version = *spoken});
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
        return finished(in, Moved{.sequence = *sequence, .corrections = *corrections, .at = *at});
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
    case Kind::Woodlands:
        return read_woodlands(in);
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
