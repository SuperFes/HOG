#include <hog/core/bytes.hpp>
#include <hog/core/hash.hpp>
#include <hog/login/service.hpp>

#include <algorithm>
#include <format>
#include <type_traits>
#include <utility>

namespace hog::login {

namespace {

// NOLINTNEXTLINE(performance-enum-size): a u16 on the wire
enum class Kind : std::uint16_t { Ask = 1, Granted = 2, Denied = 3 };

constexpr std::size_t most_token = 1'024;
constexpr std::size_t most_reason = 256;

void write_sized(ByteWriter& out, std::span<const std::byte> bytes) {
    out.write(static_cast<std::uint16_t>(bytes.size()));
    out.write_bytes(bytes);
}

[[nodiscard]] std::span<const std::byte> text_bytes(std::string_view text, std::size_t most) {
    return std::as_bytes(std::span{text.substr(0, std::min(text.size(), most))});
}

} // namespace

std::vector<std::byte> encode(const LoginMessage& message) {
    ByteWriter out;
    std::visit(
        [&](const auto& body) {
            using Body = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<Body, Ask>) {
                out.write(static_cast<std::uint16_t>(Kind::Ask));
                out.write(body.version);
                write_sized(out, text_bytes(body.name, most_name));
            } else if constexpr (std::is_same_v<Body, Granted>) {
                out.write(static_cast<std::uint16_t>(Kind::Granted));
                write_sized(out,
                            std::span{body.token}.first(std::min(body.token.size(), most_token)));
            } else {
                out.write(static_cast<std::uint16_t>(Kind::Denied));
                write_sized(out, text_bytes(body.reason, most_reason));
            }
        },
        message);
    return std::move(out).take();
}

Result<LoginMessage> decode(std::span<const std::byte> bytes) {
    ValueReader in{bytes};
    const auto kind = in.integer<std::uint16_t>();
    LoginMessage message;
    switch (static_cast<Kind>(kind)) {
    case Kind::Ask: {
        Ask ask;
        ask.version = in.integer<std::uint32_t>();
        const auto length = in.integer<std::uint16_t>();
        if (length > most_name) {
            return make_error(ErrorCode::Parse, "a name too long");
        }
        ask.name = in.text(length);
        message = std::move(ask);
        break;
    }
    case Kind::Granted: {
        const auto length = in.integer<std::uint16_t>();
        if (length > most_token) {
            return make_error(ErrorCode::Parse, "a token too long");
        }
        Granted granted;
        for (const char byte : in.text(length)) {
            granted.token.push_back(static_cast<std::byte>(byte));
        }
        message = std::move(granted);
        break;
    }
    case Kind::Denied: {
        const auto length = in.integer<std::uint16_t>();
        if (length > most_reason) {
            return make_error(ErrorCode::Parse, "a reason too long");
        }
        message = Denied{.reason = in.text(length)};
        break;
    }
    default:
        if (in.ok()) {
            return make_error(ErrorCode::Unsupported,
                              std::format("unknown login message {}", kind));
        }
    }
    if (auto read = in.finish(); !read) {
        return make_error(ErrorCode::Parse,
                          std::format("a malformed login message: {}", read.error().message));
    }
    return message;
}

bool fit_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > most_name || name.front() == ' ' || name.back() == ' ') {
        return false;
    }
    return std::ranges::all_of(name, [](char letter) {
        return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
               (letter >= '0' && letter <= '9') || letter == ' ' || letter == '_' || letter == '-';
    });
}

LoginMessage answer(std::span<const std::byte> asked, const KeyPair& keys, UnixTime now,
                    const Seed& nonce) {
    const auto message = decode(asked);
    if (!message) {
        return Denied{.reason =
                          std::format("couldn't read the login: {}", message.error().message)};
    }
    const auto* ask = std::get_if<Ask>(&*message);
    if (ask == nullptr) {
        return Denied{.reason = "a login must ask for a token"};
    }
    if (ask->version != login_version) {
        return Denied{.reason = std::format("the login service speaks version {} and this "
                                            "client {}",
                                            login_version, ask->version)};
    }
    if (!fit_name(ask->name)) {
        return Denied{.reason = std::format("\"{}\" isn't a name: 1 to {} letters, digits, spaces, "
                                            "'_' or '-'",
                                            ask->name, most_name)};
    }
    Token token{.account = hash_string(ask->name),
                .name = ask->name,
                .issued = now,
                .expires = now + token_life,
                .nonce = {}};
    std::ranges::copy(std::span{nonce}.first(token.nonce.size()), token.nonce.begin());
    return Granted{.token = sign(token, keys)};
}

} // namespace hog::login
