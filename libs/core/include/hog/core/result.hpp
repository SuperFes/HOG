#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace hog {

// Broad failure classes callers can branch on; the message carries the detail. Extend as modules
// need new classes rather than encoding them in messages.
enum class ErrorCode : std::uint8_t {
    InvalidArgument,
    NotFound,
    OutOfRange,
    Io,
    Parse,
    Unsupported,
    Internal,
};

[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidArgument:
        return "invalid argument";
    case ErrorCode::NotFound:
        return "not found";
    case ErrorCode::OutOfRange:
        return "out of range";
    case ErrorCode::Io:
        return "i/o error";
    case ErrorCode::Parse:
        return "parse error";
    case ErrorCode::Unsupported:
        return "unsupported";
    case ErrorCode::Internal:
        return "internal error";
    }
    return "unknown error";
}

struct Error {
    ErrorCode code;
    std::string message;

    friend bool operator==(const Error&, const Error&) = default;
};

template <typename T = void>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> make_error(ErrorCode code, std::string message) {
    return std::unexpected(Error{.code = code, .message = std::move(message)});
}

} // namespace hog

template <>
struct std::formatter<hog::Error> : std::formatter<std::string_view> {
    // Reuses the string_view formatter so width and fill specs apply to the whole text.
    auto format(const hog::Error& error, std::format_context& context) const {
        return std::formatter<std::string_view>::format(
            std::format("{}: {}", hog::to_string(error.code), error.message), context);
    }
};
