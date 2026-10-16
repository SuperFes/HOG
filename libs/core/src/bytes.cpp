#include <hog/core/bytes.hpp>

#include <format>

namespace hog {

namespace {

constexpr std::uint8_t more = 0x80;
constexpr std::uint8_t low_seven = 0x7F;

} // namespace

void ByteWriter::write_varint(std::uint64_t value) {
    while (value > low_seven) {
        out_.push_back(static_cast<std::byte>((value & low_seven) | more));
        value >>= 7U;
    }
    out_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::write_signed(std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    write_varint((bits << 1U) ^ (value < 0 ? ~std::uint64_t{0} : 0));
}

Result<std::span<const std::byte>> ByteReader::read_bytes(std::size_t count) {
    if (bytes_.size() < count) {
        return make_error(ErrorCode::Parse,
                          std::format("needed {} bytes, {} left", count, bytes_.size()));
    }
    const auto read = bytes_.first(count);
    bytes_ = bytes_.subspan(count);
    return read;
}

Result<std::uint64_t> ByteReader::read_varint() {
    std::uint64_t value = 0;
    std::size_t used = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (used == bytes_.size()) {
            return make_error(ErrorCode::Parse, "varint cut short");
        }
        const auto byte = std::to_integer<std::uint8_t>(bytes_[used++]);
        // The tenth byte holds only the top bit.
        if (shift == 63 && byte > 1) {
            break;
        }
        value |= static_cast<std::uint64_t>(byte & low_seven) << shift;
        if ((byte & more) == 0) {
            bytes_ = bytes_.subspan(used);
            return value;
        }
    }
    return make_error(ErrorCode::Parse, "varint too large for 64 bits");
}

Result<std::int64_t> ByteReader::read_signed() {
    const auto bits = read_varint();
    if (!bits) {
        return std::unexpected(bits.error());
    }
    return static_cast<std::int64_t>((*bits >> 1U) ^ (~(*bits & 1U) + 1U));
}

Result<void> ByteReader::finish() const {
    if (!bytes_.empty()) {
        return make_error(ErrorCode::Parse, std::format("{} unread bytes", bytes_.size()));
    }
    return {};
}

} // namespace hog
