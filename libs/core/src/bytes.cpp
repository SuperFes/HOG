#include <hog/core/bytes.hpp>

#include <format>

namespace hog {

Result<std::span<const std::byte>> ByteReader::read_bytes(std::size_t count) {
    if (bytes_.size() < count) {
        return make_error(ErrorCode::Parse,
                          std::format("needed {} bytes, {} left", count, bytes_.size()));
    }
    const auto read = bytes_.first(count);
    bytes_ = bytes_.subspan(count);
    return read;
}

Result<void> ByteReader::finish() const {
    if (!bytes_.empty()) {
        return make_error(ErrorCode::Parse, std::format("{} unread bytes", bytes_.size()));
    }
    return {};
}

} // namespace hog
