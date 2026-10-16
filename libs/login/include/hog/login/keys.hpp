#pragma once

#include <hog/core/result.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Ed25519 keys connect tokens are signed and checked with.
namespace hog::login {

using Seed = std::array<std::uint8_t, 32>;
using PublicKey = std::array<std::uint8_t, 32>;

struct KeyPair {
    std::array<std::uint8_t, 64> secret{}; // Ed25519's: the seed, then the public key
    PublicKey public_key{};
};

// 32 bytes from the system's source of randomness, fit for keys and nonces.
[[nodiscard]] Result<Seed> random_seed();

[[nodiscard]] KeyPair key_pair(const Seed& seed);

[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);
// Exactly `size` bytes, from twice as many hex digits.
[[nodiscard]] Result<std::vector<std::uint8_t>> from_hex(std::string_view text, std::size_t size);

// The key pair whose seed is kept, in hex, at `seed_file`, made and saved there first if there
// isn't one; its public key is written, in hex, to `public_file` either way.
[[nodiscard]] Result<KeyPair> load_or_make_keys(const std::filesystem::path& seed_file,
                                                const std::filesystem::path& public_file);
// A public key saved by load_or_make_keys.
[[nodiscard]] Result<PublicKey> load_public_key(const std::filesystem::path& file);

} // namespace hog::login
