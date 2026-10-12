#include <hog/core/assert.hpp>
#include <hog/core/bytes.hpp>
#include <hog/probe/messages.hpp>

#include <algorithm>
#include <bit>
#include <format>
#include <limits>
#include <utility>

namespace hog::probe {

namespace {

// Kept apart from the variant's order, which may change without breaking the wire.
enum class Kind : std::uint8_t {
    Hello = 1,
    Welcome = 2,
    Refused = 3,
    Fetch = 4,
    Layers = 5,
    Control = 6,
    Controlled = 7,
    Survey = 8,
    Ground = 9,
    Inspect = 10,
    Inspection = 11,
};

constexpr std::size_t header_size = sizeof(std::uint32_t) + sizeof(std::uint8_t);

constexpr Kind kind_of(const Hello& /*unused*/) noexcept {
    return Kind::Hello;
}
constexpr Kind kind_of(const Welcome& /*unused*/) noexcept {
    return Kind::Welcome;
}
constexpr Kind kind_of(const Refused& /*unused*/) noexcept {
    return Kind::Refused;
}
constexpr Kind kind_of(const Fetch& /*unused*/) noexcept {
    return Kind::Fetch;
}
constexpr Kind kind_of(const Layers& /*unused*/) noexcept {
    return Kind::Layers;
}
constexpr Kind kind_of(const Control& /*unused*/) noexcept {
    return Kind::Control;
}
constexpr Kind kind_of(const Controlled& /*unused*/) noexcept {
    return Kind::Controlled;
}
constexpr Kind kind_of(const Survey& /*unused*/) noexcept {
    return Kind::Survey;
}
constexpr Kind kind_of(const Ground& /*unused*/) noexcept {
    return Kind::Ground;
}
constexpr Kind kind_of(const Inspect& /*unused*/) noexcept {
    return Kind::Inspect;
}
constexpr Kind kind_of(const Inspection& /*unused*/) noexcept {
    return Kind::Inspection;
}

void write_text(ByteWriter& out, const std::string& text) {
    HOG_ASSERT(text.size() <= std::numeric_limits<std::uint16_t>::max(), "text too long to send");
    out.write(static_cast<std::uint16_t>(text.size()));
    out.write_bytes(std::as_bytes(std::span{text}));
}

void write_count(ByteWriter& out, std::size_t count) {
    HOG_ASSERT(count <= std::numeric_limits<std::uint32_t>::max(), "too many to send");
    out.write(static_cast<std::uint32_t>(count));
}

void write_flag(ByteWriter& out, bool flag) {
    out.write(static_cast<std::uint8_t>(flag ? 1 : 0));
}

void write_clock(ByteWriter& out, const Clock& clock) {
    write_flag(out, clock.paused);
    out.write(clock.scale);
}

void write_body(ByteWriter& out, const Hello& hello) {
    out.write(hello.version);
}

void write_body(ByteWriter& out, const Welcome& welcome) {
    out.write(welcome.version);
    out.write(welcome.world_id);
    out.write(welcome.level);
    out.write(std::bit_cast<std::uint64_t>(welcome.planet_radius));
    write_count(out, welcome.layers.size());
    for (const LayerInfo& layer : welcome.layers) {
        write_text(out, layer.name);
        write_text(out, layer.unit);
    }
    write_flag(out, welcome.controllable);
    out.write(static_cast<std::uint64_t>(welcome.day_length.count()));
    out.write(std::bit_cast<std::uint64_t>(welcome.axial_tilt));
}

void write_body(ByteWriter& out, const Refused& refused) {
    write_text(out, refused.reason);
}

void write_body(ByteWriter& out, const Fetch& fetch) {
    write_count(out, fetch.names.size());
    for (const std::string& name : fetch.names) {
        write_text(out, name);
    }
}

void write_body(ByteWriter& out, const Layers& layers) {
    out.write(static_cast<std::uint64_t>(layers.time.time_since_epoch().count()));
    write_clock(out, layers.clock);
    write_count(out, layers.layers.size());
    for (const Layer& layer : layers.layers) {
        write_text(out, layer.name);
        write_count(out, layer.values.size());
        for (const float value : layer.values) {
            out.write(std::bit_cast<std::uint32_t>(value));
        }
    }
}

void write_body(ByteWriter& out, const Control& control) {
    out.write(std::to_underlying(control.action));
    out.write(control.amount);
}

void write_body(ByteWriter& out, const Controlled& controlled) {
    write_clock(out, controlled.clock);
    write_text(out, controlled.error);
}

void write_body(ByteWriter& out, const Survey& survey) {
    for (const double edge : {survey.west, survey.east, survey.north, survey.south}) {
        out.write(std::bit_cast<std::uint64_t>(edge));
    }
    out.write(survey.width);
    out.write(survey.height);
    write_flag(out, survey.underground);
    out.write(std::bit_cast<std::uint32_t>(survey.depth));
}

void write_bytes(ByteWriter& out, const std::vector<std::uint8_t>& values) {
    write_count(out, values.size());
    out.write_bytes(std::as_bytes(std::span{values}));
}

void write_body(ByteWriter& out, const Ground& ground) {
    write_body(out, ground.survey);
    write_count(out, ground.elevation.size());
    for (const float value : ground.elevation) {
        out.write(std::bit_cast<std::uint32_t>(value));
    }
    write_bytes(out, ground.biome);
    write_bytes(out, ground.rock);
    write_bytes(out, ground.deposit);
}

void write_body(ByteWriter& out, const Inspect& inspect) {
    out.write(std::bit_cast<std::uint64_t>(inspect.latitude));
    out.write(std::bit_cast<std::uint64_t>(inspect.longitude));
}

void write_single(ByteWriter& out, float value) {
    out.write(std::bit_cast<std::uint32_t>(value));
}

void write_column(ByteWriter& out, const Column& column) {
    write_single(out, column.elevation);
    out.write(column.biome);
    write_flag(out, column.continental);
    out.write(column.cover);
    write_single(out, column.cover_thickness);
    out.write(column.sediment);
    write_single(out, column.sediment_thickness);
    out.write(column.bedrock);
    write_count(out, column.seams.size());
    for (const Seam& seam : column.seams) {
        out.write(seam.resource);
        write_single(out, seam.top);
        write_single(out, seam.thickness);
    }
}

void write_body(ByteWriter& out, const Inspection& inspection) {
    write_body(out, inspection.inspect);
    out.write(static_cast<std::uint64_t>(inspection.time.time_since_epoch().count()));
    out.write(inspection.cell);
    write_count(out, inspection.values.size());
    for (const float value : inspection.values) {
        write_single(out, value);
    }
    write_column(out, inspection.column);
}

std::string read_text(ValueReader& in) {
    return in.text(in.integer<std::uint16_t>());
}

// A count of things at least `smallest` bytes each, failing if there can't be that many left.
std::uint32_t read_count(ValueReader& in, std::size_t smallest) {
    const auto count = in.integer<std::uint32_t>();
    if (in.ok() && count > in.remaining() / smallest) {
        in.fail(Error{.code = ErrorCode::Parse,
                      .message =
                          std::format("{} entries can't fit in {} bytes", count, in.remaining())});
        return 0;
    }
    return count;
}

bool read_flag(ValueReader& in) {
    const auto flag = in.integer<std::uint8_t>();
    if (flag > 1) {
        in.fail(Error{.code = ErrorCode::Parse, .message = std::format("{} isn't a flag", flag)});
    }
    return flag == 1;
}

Clock read_clock(ValueReader& in) {
    Clock clock;
    clock.paused = read_flag(in);
    clock.scale = in.integer<std::uint32_t>();
    return clock;
}

Hello read_hello(ValueReader& in) {
    return Hello{.version = in.integer<std::uint16_t>()};
}

Welcome read_welcome(ValueReader& in) {
    Welcome welcome;
    welcome.version = in.integer<std::uint16_t>();
    welcome.world_id = in.integer<std::uint64_t>();
    welcome.level = in.integer<std::uint8_t>();
    welcome.planet_radius = in.twice();
    const std::uint32_t count = read_count(in, 2 * sizeof(std::uint16_t));
    welcome.layers.reserve(count);
    for (std::uint32_t layer = 0; layer < count && in.ok(); ++layer) {
        std::string name = read_text(in);
        welcome.layers.push_back(LayerInfo{.name = std::move(name), .unit = read_text(in)});
    }
    welcome.controllable = read_flag(in);
    welcome.day_length = GameDuration{static_cast<GameDuration::rep>(in.integer<std::uint64_t>())};
    welcome.axial_tilt = in.twice();
    return welcome;
}

Refused read_refused(ValueReader& in) {
    return Refused{.reason = read_text(in)};
}

Fetch read_fetch(ValueReader& in) {
    Fetch fetch;
    const std::uint32_t count = read_count(in, sizeof(std::uint16_t));
    fetch.names.reserve(count);
    for (std::uint32_t name = 0; name < count && in.ok(); ++name) {
        fetch.names.push_back(read_text(in));
    }
    return fetch;
}

Layers read_layers(ValueReader& in) {
    Layers layers;
    layers.time =
        GameTime{GameDuration{static_cast<GameDuration::rep>(in.integer<std::uint64_t>())}};
    layers.clock = read_clock(in);
    const std::uint32_t count = read_count(in, sizeof(std::uint16_t) + sizeof(std::uint32_t));
    layers.layers.reserve(count);
    for (std::uint32_t index = 0; index < count && in.ok(); ++index) {
        Layer layer{.name = read_text(in), .values = {}};
        layer.values.resize(read_count(in, sizeof(std::uint32_t)));
        for (float& value : layer.values) {
            value = in.single();
        }
        layers.layers.push_back(std::move(layer));
    }
    return layers;
}

Control read_control(ValueReader& in) {
    const auto action = in.integer<std::uint8_t>();
    if (action < std::to_underlying(Action::Pause) ||
        action > std::to_underlying(Action::RealTime)) {
        in.fail(
            Error{.code = ErrorCode::Parse, .message = std::format("unknown action {}", action)});
    }
    return Control{.action = static_cast<Action>(action), .amount = in.integer<std::uint32_t>()};
}

Controlled read_controlled(ValueReader& in) {
    const Clock clock = read_clock(in);
    return Controlled{.clock = clock, .error = read_text(in)};
}

Survey read_survey(ValueReader& in) {
    Survey survey;
    survey.west = in.twice();
    survey.east = in.twice();
    survey.north = in.twice();
    survey.south = in.twice();
    survey.width = in.integer<std::uint16_t>();
    survey.height = in.integer<std::uint16_t>();
    survey.underground = read_flag(in);
    survey.depth = in.single();
    return survey;
}

std::vector<std::uint8_t> read_bytes(ValueReader& in) {
    std::vector<std::uint8_t> values(read_count(in, sizeof(std::uint8_t)));
    for (std::uint8_t& value : values) {
        value = in.integer<std::uint8_t>();
    }
    return values;
}

Ground read_ground(ValueReader& in) {
    Ground ground{
        .survey = read_survey(in), .elevation = {}, .biome = {}, .rock = {}, .deposit = {}};
    ground.elevation.resize(read_count(in, sizeof(std::uint32_t)));
    for (float& value : ground.elevation) {
        value = in.single();
    }
    ground.biome = read_bytes(in);
    ground.rock = read_bytes(in);
    ground.deposit = read_bytes(in);
    const std::size_t samples =
        static_cast<std::size_t>(ground.survey.width) * ground.survey.height;
    const std::size_t below = ground.survey.underground ? samples : 0;
    if (in.ok() && (ground.elevation.size() != samples || ground.biome.size() != samples ||
                    ground.rock.size() != below || ground.deposit.size() != below)) {
        in.fail(Error{.code = ErrorCode::Parse,
                      .message = std::format("ground values don't match its {} by {} samples",
                                             ground.survey.width, ground.survey.height)});
    }
    return ground;
}

Inspect read_inspect(ValueReader& in) {
    Inspect inspect;
    inspect.latitude = in.twice();
    inspect.longitude = in.twice();
    return inspect;
}

Column read_column(ValueReader& in) {
    Column column;
    column.elevation = in.single();
    column.biome = in.integer<std::uint8_t>();
    column.continental = read_flag(in);
    column.cover = in.integer<std::uint8_t>();
    column.cover_thickness = in.single();
    column.sediment = in.integer<std::uint8_t>();
    column.sediment_thickness = in.single();
    column.bedrock = in.integer<std::uint8_t>();
    const std::uint32_t count = read_count(in, sizeof(std::uint8_t) + (2 * sizeof(float)));
    column.seams.reserve(count);
    for (std::uint32_t index = 0; index < count && in.ok(); ++index) {
        Seam seam;
        seam.resource = in.integer<std::uint8_t>();
        seam.top = in.single();
        seam.thickness = in.single();
        column.seams.push_back(seam);
    }
    return column;
}

Inspection read_inspection(ValueReader& in) {
    Inspection inspection;
    inspection.inspect = read_inspect(in);
    inspection.time =
        GameTime{GameDuration{static_cast<GameDuration::rep>(in.integer<std::uint64_t>())}};
    inspection.cell = in.integer<std::uint32_t>();
    inspection.values.resize(read_count(in, sizeof(std::uint32_t)));
    for (float& value : inspection.values) {
        value = in.single();
    }
    inspection.column = read_column(in);
    return inspection;
}

Result<Message> decode(Kind kind, std::span<const std::byte> payload) {
    ValueReader in{payload};
    Message message;
    switch (kind) {
    case Kind::Hello:
        message = read_hello(in);
        break;
    case Kind::Welcome:
        message = read_welcome(in);
        break;
    case Kind::Refused:
        message = read_refused(in);
        break;
    case Kind::Fetch:
        message = read_fetch(in);
        break;
    case Kind::Layers:
        message = read_layers(in);
        break;
    case Kind::Control:
        message = read_control(in);
        break;
    case Kind::Controlled:
        message = read_controlled(in);
        break;
    case Kind::Survey:
        message = read_survey(in);
        break;
    case Kind::Ground:
        message = read_ground(in);
        break;
    case Kind::Inspect:
        message = read_inspect(in);
        break;
    case Kind::Inspection:
        message = read_inspection(in);
        break;
    default:
        return make_error(ErrorCode::Parse,
                          std::format("unknown message kind {}", std::to_underlying(kind)));
    }
    if (auto finished = in.finish(); !finished) {
        return std::unexpected(std::move(finished.error()));
    }
    return message;
}

} // namespace

std::vector<std::byte> encode(const Message& message) {
    ByteWriter body;
    const Kind kind = std::visit(
        [&body](const auto& value) {
            write_body(body, value);
            return kind_of(value);
        },
        message);
    HOG_ASSERT(body.size() <= max_payload, "message too large to send");
    ByteWriter frame;
    frame.write(static_cast<std::uint32_t>(body.size()));
    frame.write(std::to_underlying(kind));
    frame.write_bytes(body.bytes());
    return std::move(frame).take();
}

void FrameReader::feed(std::span<const std::byte> bytes) {
    if (start_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(start_));
        start_ = 0;
    }
    const std::size_t end = buffer_.size();
    buffer_.resize(end + bytes.size());
    std::ranges::copy(bytes, std::span{buffer_}.subspan(end).begin());
}

Result<std::optional<Message>> FrameReader::next() {
    if (error_) {
        return std::unexpected(*error_);
    }
    const std::span<const std::byte> unread = std::span{buffer_}.subspan(start_);
    if (unread.size() < header_size) {
        return std::nullopt;
    }
    ByteReader header{unread.first(header_size)};
    const std::uint32_t length = *header.read<std::uint32_t>();
    const auto kind = static_cast<Kind>(*header.read<std::uint8_t>());
    if (length > max_payload) {
        error_ = Error{.code = ErrorCode::Parse,
                       .message = std::format("a {} byte message is over the {} byte limit", length,
                                              max_payload)};
        return std::unexpected(*error_);
    }
    if (unread.size() - header_size < length) {
        return std::nullopt;
    }
    auto message = decode(kind, unread.subspan(header_size, length));
    if (!message) {
        error_ = std::move(message.error());
        return std::unexpected(*error_);
    }
    start_ += header_size + length;
    return std::optional<Message>{std::move(*message)};
}

} // namespace hog::probe
