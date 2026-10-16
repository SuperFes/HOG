#include <hog/core/text_file.hpp>
#include <hog/login/keys.hpp>

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <format>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#elif defined(__APPLE__)
#include <cstdlib>
#else
#include <sys/random.h>
#endif

namespace hog::login {

namespace {

constexpr std::string_view digits = "0123456789abcdef";

[[nodiscard]] Result<void> write_text(const std::filesystem::path& file, std::string_view text) {
    std::error_code made;
    std::filesystem::create_directories(file.parent_path(), made);
    std::ofstream out{file, std::ios::trunc};
    out << text << '\n';
    out.close();
    if (!out) {
        return make_error(ErrorCode::Io, std::format("couldn't write {}", file.string()));
    }
    return {};
}

} // namespace

Result<Seed> random_seed() {
    Seed seed{};
#if defined(_WIN32)
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, seed.data(), static_cast<ULONG>(seed.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return make_error(ErrorCode::Io, "the system's randomness failed");
    }
#elif defined(__APPLE__)
    arc4random_buf(seed.data(), seed.size());
#else
    std::size_t filled = 0;
    while (filled < seed.size()) {
        const std::span<std::uint8_t> rest = std::span{seed}.subspan(filled);
        const ssize_t got = getrandom(rest.data(), rest.size(), 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return make_error(ErrorCode::Io, std::format("the system's randomness failed: {}",
                                                         std::generic_category().message(errno)));
        }
        filled += static_cast<std::size_t>(got);
    }
#endif
    return seed;
}

KeyPair key_pair(const Seed& seed) {
    KeyPair keys;
    // Monocypher wipes the seed it's given.
    Seed wiped = seed;
    crypto_ed25519_key_pair(keys.secret.data(), keys.public_key.data(), wiped.data());
    return keys;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0xFU]);
    }
    return text;
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view text, std::size_t size) {
    if (text.size() != size * 2) {
        return make_error(ErrorCode::Parse,
                          std::format("expected {} hex digits, not {}", size * 2, text.size()));
    }
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t at = 0; at < size; ++at) {
        const auto high = digits.find(static_cast<char>(std::tolower(text[2 * at])));
        const auto low = digits.find(static_cast<char>(std::tolower(text[(2 * at) + 1])));
        if (high == std::string_view::npos || low == std::string_view::npos) {
            return make_error(ErrorCode::Parse, "not hex digits");
        }
        bytes[at] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return bytes;
}

Result<KeyPair> load_or_make_keys(const std::filesystem::path& seed_file,
                                  const std::filesystem::path& public_file) {
    Seed seed{};
    if (std::filesystem::exists(seed_file)) {
        const auto text = read_text_file(seed_file);
        if (!text) {
            return std::unexpected(text.error());
        }
        const std::string_view trimmed =
            std::string_view{*text}.substr(0, std::min(text->find_first_of("\r\n"), text->size()));
        const auto bytes = from_hex(trimmed, seed.size());
        if (!bytes) {
            return make_error(ErrorCode::Parse,
                              std::format("{}: {}", seed_file.string(), bytes.error().message));
        }
        std::ranges::copy(*bytes, seed.begin());
    } else {
        auto made = random_seed();
        if (!made) {
            return std::unexpected(made.error());
        }
        seed = *made;
        if (auto saved = write_text(seed_file, to_hex(seed)); !saved) {
            return std::unexpected(saved.error());
        }
        // Only its owner should read the seed.
        std::error_code ignored;
        std::filesystem::permissions(
            seed_file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            ignored);
    }
    KeyPair keys = key_pair(seed);
    crypto_wipe(seed.data(), seed.size());
    if (auto saved = write_text(public_file, to_hex(keys.public_key)); !saved) {
        return std::unexpected(saved.error());
    }
    return keys;
}

Result<PublicKey> load_public_key(const std::filesystem::path& file) {
    const auto text = read_text_file(file);
    if (!text) {
        return std::unexpected(text.error());
    }
    const std::string_view trimmed =
        std::string_view{*text}.substr(0, std::min(text->find_first_of("\r\n"), text->size()));
    PublicKey key{};
    const auto bytes = from_hex(trimmed, key.size());
    if (!bytes) {
        return make_error(ErrorCode::Parse,
                          std::format("{}: {}", file.string(), bytes.error().message));
    }
    std::ranges::copy(*bytes, key.begin());
    return key;
}

} // namespace hog::login
