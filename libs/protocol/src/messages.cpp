#include <hog/core/assert.hpp>
#include <hog/core/bytes.hpp>
#include <hog/protocol/messages.hpp>

#include <algorithm>
#include <format>
#include <string_view>
#include <type_traits>
#include <utility>

namespace hog::protocol {

namespace {

// A u16 on the wire, leaving room for many more.
// NOLINTNEXTLINE(performance-enum-size)
enum class Kind : std::uint16_t {
    Hello = 1,
    Welcome = 2,
    Refused = 3
}; // NOLINT(performance-enum-size)

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

void write_sized(ByteWriter& out, std::span<const std::byte> bytes) {
    out.write(static_cast<std::uint16_t>(bytes.size()));
    out.write_bytes(bytes);
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

} // namespace

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
            } else {
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
