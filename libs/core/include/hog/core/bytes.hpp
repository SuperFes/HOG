#pragma once

#include <hog/core/result.hpp>

#include <concepts>
#include <cstddef>
#include <span>
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

    void write_bytes(std::span<const std::byte> bytes) {
        out_.insert(out_.end(), bytes.begin(), bytes.end());
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

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size(); }

    // Fails if any bytes are left unread.
    [[nodiscard]] Result<void> finish() const;

private:
    std::span<const std::byte> bytes_;
};

} // namespace hog
