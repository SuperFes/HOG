#include <hog/core/bytes.hpp>
#include <hog/login/token.hpp>

#include <monocypher-ed25519.h>

#include <algorithm>
#include <format>

namespace hog::login {

namespace {

constexpr std::size_t signature_size = 64;

std::vector<std::uint8_t> octets(std::span<const std::byte> bytes) {
    std::vector<std::uint8_t> out(bytes.size());
    std::ranges::transform(bytes, out.begin(),
                           [](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
    return out;
}

std::uint64_t seconds_of(UnixTime time) {
    return static_cast<std::uint64_t>(time.time_since_epoch().count());
}

UnixTime time_of(std::uint64_t seconds) {
    return UnixTime{std::chrono::seconds{static_cast<std::int64_t>(seconds)}};
}

} // namespace

std::vector<std::byte> sign(const Token& token, const KeyPair& keys) {
    ByteWriter out;
    out.write(token_version);
    out.write(token.account);
    const std::size_t name = std::min(token.name.size(), most_name);
    out.write(static_cast<std::uint8_t>(name));
    out.write_bytes(std::as_bytes(std::span{token.name}.first(name)));
    out.write(seconds_of(token.issued));
    out.write(seconds_of(token.expires));
    for (const std::uint8_t byte : token.nonce) {
        out.write(byte);
    }
    const std::vector<std::uint8_t> message = octets(out.bytes());
    std::array<std::uint8_t, signature_size> signature{};
    crypto_ed25519_sign(signature.data(), keys.secret.data(), message.data(), message.size());
    for (const std::uint8_t byte : signature) {
        out.write(byte);
    }
    return std::move(out).take();
}

Result<Token> check(std::span<const std::byte> bytes, const PublicKey& key, UnixTime now) {
    if (bytes.size() <= signature_size) {
        return make_error(ErrorCode::Parse, "a token too short to be one");
    }
    const auto signed_part = bytes.first(bytes.size() - signature_size);
    const std::vector<std::uint8_t> message = octets(signed_part);
    const std::vector<std::uint8_t> signature = octets(bytes.last(signature_size));
    ValueReader in{signed_part};
    const auto version = in.integer<std::uint8_t>();
    if (in.ok() && version != token_version) {
        return make_error(ErrorCode::Unsupported,
                          std::format("a token of version {}, not {}", version, token_version));
    }
    if (crypto_ed25519_check(signature.data(), key.data(), message.data(), message.size()) != 0) {
        return make_error(ErrorCode::InvalidArgument, "a token not signed by the login service");
    }
    Token token;
    token.account = in.integer<std::uint64_t>();
    const auto name = in.integer<std::uint8_t>();
    if (name > most_name) {
        return make_error(ErrorCode::Parse, "a token's name too long");
    }
    token.name = in.text(name);
    token.issued = time_of(in.integer<std::uint64_t>());
    token.expires = time_of(in.integer<std::uint64_t>());
    for (std::uint8_t& byte : token.nonce) {
        byte = in.integer<std::uint8_t>();
    }
    if (auto read = in.finish(); !read) {
        return make_error(ErrorCode::Parse,
                          std::format("a malformed token: {}", read.error().message));
    }
    if (now >= token.expires) {
        return make_error(ErrorCode::OutOfRange, "an expired token");
    }
    if (token.issued > now + clock_slack) {
        return make_error(ErrorCode::OutOfRange, "a token issued in the future");
    }
    return token;
}

} // namespace hog::login
