#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Hashes whose output is fixed forever: identical on every platform, compiler and build, so they
// can feed world generation, golden tests and anything stored or sent over the network. Never use
// std::hash for those; its output is unspecified and differs between standard libraries.
namespace hog {

// The splitmix64 finalizer: a fixed bijective 64-bit mixer.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] constexpr std::uint64_t hash_combine(std::uint64_t seed,
                                                   std::uint64_t value) noexcept {
    return mix64(seed ^ (value + 0x9E3779B97F4A7C15ULL));
}

// Hashes integers in order. Signed values hash as their two's-complement bits.
template <std::integral... Values>
[[nodiscard]] constexpr std::uint64_t hash_values(std::uint64_t seed, Values... values) noexcept {
    ((seed = hash_combine(seed, static_cast<std::uint64_t>(values))), ...);
    return seed;
}

// XXH3-64 of the bytes.
[[nodiscard]] std::uint64_t hash_bytes(std::span<const std::byte> bytes,
                                       std::uint64_t seed = 0) noexcept;

// XXH3-64 of the string's bytes (UTF-8 for text).
[[nodiscard]] std::uint64_t hash_string(std::string_view text, std::uint64_t seed = 0) noexcept;

} // namespace hog
