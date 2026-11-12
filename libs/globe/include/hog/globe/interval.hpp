#pragma once

#include <algorithm>

namespace hog::globe {

// Bounds of a value over somewhere: every value it takes there lies from `low` to `high`.
struct Interval {
    double low = 0.0;
    double high = 0.0;

    friend constexpr Interval operator+(Interval one, Interval other) noexcept {
        return {.low = one.low + other.low, .high = one.high + other.high};
    }
    friend constexpr Interval operator*(double scale, Interval one) noexcept {
        return {.low = std::min(scale * one.low, scale * one.high),
                .high = std::max(scale * one.low, scale * one.high)};
    }
    friend constexpr Interval operator*(Interval one, Interval other) noexcept {
        const double a = one.low * other.low;
        const double b = one.low * other.high;
        const double c = one.high * other.low;
        const double d = one.high * other.high;
        return {.low = std::min({a, b, c, d}), .high = std::max({a, b, c, d})};
    }
    [[nodiscard]] constexpr bool contains(double value) const noexcept {
        return value >= low && value <= high;
    }
};

// Both together.
[[nodiscard]] constexpr Interval hull(Interval one, Interval other) noexcept {
    return {.low = std::min(one.low, other.low), .high = std::max(one.high, other.high)};
}

// Where both hold; both must bound the same value.
[[nodiscard]] constexpr Interval meet(Interval one, Interval other) noexcept {
    return {.low = std::max(one.low, other.low), .high = std::min(one.high, other.high)};
}

} // namespace hog::globe
