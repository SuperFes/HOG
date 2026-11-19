#pragma once

#include <hog/core/result.hpp>
#include <hog/grid/kinds.hpp>

#include <lmdb++.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace hog::baked::detail {

inline std::string_view as_view(std::span<const std::byte> bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

inline std::span<const std::byte> as_bytes(std::string_view view) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const std::byte*>(view.data()), view.size()};
}

// The value under `key`, read in place; LMDB's own reads are const in all but name.
inline bool get(const lmdb::txn& txn, const lmdb::dbi& dbi, std::string_view key,
                std::string_view& value) {
    MDB_val key_val{key.size(), const_cast<char*>(key.data())}; // NOLINT: LMDB takes it mutable
    MDB_val value_val{};
    if (!lmdb::dbi_get(txn.handle(), dbi.handle(), &key_val, &value_val)) {
        return false;
    }
    value = {static_cast<const char*>(value_val.mv_data), value_val.mv_size};
    return true;
}

// Runs `body`, turning what LMDB throws into an error.
template <typename Body>
auto guarded(const Body& body) -> decltype(body()) {
    try {
        return body();
    } catch (const lmdb::error& error) {
        return make_error(ErrorCode::Io, error.what());
    }
}

template <typename Kind>
constexpr std::uint8_t tag_of() noexcept {
    if constexpr (std::is_same_v<Kind, grid::Shape>) {
        return 0;
    } else if constexpr (std::is_same_v<Kind, grid::Material>) {
        return 1;
    } else if constexpr (std::is_same_v<Kind, grid::Water>) {
        return 2;
    } else {
        static_assert(std::is_same_v<Kind, grid::Woods>);
        return 3;
    }
}

// A directory removed when this is dropped.
struct Removed {
    std::filesystem::path directory;

    Removed() = default;
    Removed(Removed&& other) noexcept : directory{std::exchange(other.directory, {})} {}
    Removed& operator=(Removed&&) = delete;
    Removed(const Removed&) = delete;
    Removed& operator=(const Removed&) = delete;
    ~Removed() {
        if (!directory.empty()) {
            std::error_code failed;
            std::filesystem::remove_all(directory, failed);
        }
    }
};

// A new directory of its own under the system's temporary one.
[[nodiscard]] Result<std::filesystem::path> new_scratch_directory();

} // namespace hog::baked::detail
