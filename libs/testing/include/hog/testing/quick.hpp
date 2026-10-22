#pragma once

#include <cstddef>

namespace hog::testing {

// Whether HOG_QUICK_TESTS, set to `value`, asks the longest tests to run small: set and neither
// empty nor "0".
[[nodiscard]] bool quick_from(const char* value) noexcept;

// Whether the longest tests run small, as sanitizer builds run them between releases.
[[nodiscard]] bool quick() noexcept;

// `full`, unless the longest tests run small; then `small`.
template <typename T>
[[nodiscard]] T full_or(T full, T small) {
    return quick() ? small : full;
}

// How many of `count` cases to try: all of them, unless the longest tests run small; then one.
[[nodiscard]] inline std::size_t cases(std::size_t count) {
    return full_or(count, std::size_t{1});
}

} // namespace hog::testing
