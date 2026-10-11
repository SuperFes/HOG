#include <hog/core/hash.hpp>

// NOLINTNEXTLINE(readability-identifier-naming): xxHash configuration macro
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace hog {

std::uint64_t hash_bytes(std::span<const std::byte> bytes, std::uint64_t seed) noexcept {
    return XXH3_64bits_withSeed(bytes.data(), bytes.size(), seed);
}

std::uint64_t hash_string(std::string_view text, std::uint64_t seed) noexcept {
    return XXH3_64bits_withSeed(text.data(), text.size(), seed);
}

} // namespace hog
