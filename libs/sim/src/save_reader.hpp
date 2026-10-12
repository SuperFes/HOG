#pragma once

#include <hog/core/bytes.hpp>
#include <hog/core/result.hpp>

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace hog::sim::detail {

// Reads values in order, keeping the first failure and reading nothing after it.
class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : in_{bytes} {}

    template <std::unsigned_integral T>
    T integer() {
        if (error_) {
            return 0;
        }
        auto value = in_.read<T>();
        if (!value) {
            error_ = std::move(value.error());
            return 0;
        }
        return *value;
    }
    float single() { return std::bit_cast<float>(integer<std::uint32_t>()); }
    // Empty after a failure.
    std::string text(std::size_t length) {
        if (error_) {
            return {};
        }
        auto bytes = in_.read_bytes(length);
        if (!bytes) {
            error_ = std::move(bytes.error());
            return {};
        }
        std::string out(bytes->size(), '\0');
        std::ranges::transform(*bytes, out.begin(),
                               [](std::byte byte) { return static_cast<char>(byte); });
        return out;
    }
    void fail(Error error) {
        if (!error_) {
            error_ = std::move(error);
        }
    }
    double twice() { return std::bit_cast<double>(integer<std::uint64_t>()); }

    [[nodiscard]] bool ok() const noexcept { return !error_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return in_.remaining(); }
    // Fails with the first failure, or if bytes are left over.
    [[nodiscard]] Result<void> finish() const {
        if (error_) {
            return std::unexpected(*error_);
        }
        return in_.finish();
    }

private:
    ByteReader in_;
    std::optional<Error> error_;
};

} // namespace hog::sim::detail
