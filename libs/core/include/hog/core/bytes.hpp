#pragma once

#include <hog/core/result.hpp>

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace hog {

// Builds bytes for storage: fixed-width little-endian integers, the same on every platform.
class ByteWriter {
public:
    template <std::unsigned_integral T>
    void write(T value) {
        for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
            out_.push_back(static_cast<std::byte>(value >> (8 * byte)));
        }
    }

    // Seven bits a byte, lowest first, the top bit set while more follow, so small values take
    // fewer bytes.
    void write_varint(std::uint64_t value);
    // Zigzagged into a varint, so values near zero either side stay short.
    void write_signed(std::int64_t value);

    void write_bytes(std::span<const std::byte> bytes) {
        // Not vector::insert, which GCC 14.2 wrongly reports as overflowing at -O2.
        const std::size_t start = out_.size();
        out_.resize(start + bytes.size());
        std::ranges::copy(bytes, std::span{out_}.subspan(start).begin());
    }

    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return out_; }
    [[nodiscard]] std::vector<std::byte> take() && { return std::move(out_); }

private:
    std::vector<std::byte> out_;
};

// Reads what a ByteWriter wrote. Every read fails rather than run past the end.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> bytes) noexcept : bytes_{bytes} {}

    template <std::unsigned_integral T>
    [[nodiscard]] Result<T> read() {
        auto bytes = read_bytes(sizeof(T));
        if (!bytes) {
            return std::unexpected(std::move(bytes.error()));
        }
        T value = 0;
        for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
            value |= static_cast<T>(std::to_integer<T>((*bytes)[byte]) << (8 * byte));
        }
        return value;
    }

    [[nodiscard]] Result<std::span<const std::byte>> read_bytes(std::size_t count);

    // What write_varint and write_signed wrote; fails on one cut short or too large for 64 bits.
    [[nodiscard]] Result<std::uint64_t> read_varint();
    [[nodiscard]] Result<std::int64_t> read_signed();

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size(); }

    // Fails if any bytes are left unread.
    [[nodiscard]] Result<void> finish() const;

private:
    std::span<const std::byte> bytes_;
};

// Reads values in order, keeping the first failure and reading nothing after it.
class ValueReader {
public:
    explicit ValueReader(std::span<const std::byte> bytes) noexcept : in_{bytes} {}

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

} // namespace hog
