#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>

// Transcendental functions from +, −, ×, ÷ and sqrt only, in a fixed order. libm's last bits differ
// between platforms, and the world is regenerated rather than stored, so anything that decides
// what is where must get the same bits everywhere. Good to a few ulps; they need not be correct
// rounding, only the same.
namespace hog::globe::maths {

namespace detail {

// Horner's rule, highest power first.
template <std::size_t N>
[[nodiscard]] inline double poly(double x, const std::array<double, N>& coefficients) noexcept {
    double sum = coefficients[0];
    for (std::size_t k = 1; k < N; ++k) {
        sum = (sum * x) + coefficients[k];
    }
    return sum;
}

// π/2 in two parts, the first with its low bits clear so k times it is exact for |k| < 2^20.
inline constexpr double half_pi_high = 1.5707963267341256e+00;
inline constexpr double half_pi_low = 6.0771005065061922e-11;

struct Reduced {
    double rest; // within [-π/4, π/4]
    int quarter; // quarter turns taken off, mod 4
};

[[nodiscard]] inline Reduced reduce(double radians) noexcept {
    const double turns = std::nearbyint(radians * std::numbers::inv_pi * 2.0);
    const double rest = (radians - (turns * half_pi_high)) - (turns * half_pi_low);
    const auto quarter = static_cast<int>(std::fmod(turns, 4.0));
    return {.rest = rest, .quarter = quarter < 0 ? quarter + 4 : quarter};
}

// Taylor series on [-π/4, π/4], to below one ulp at the ends.
[[nodiscard]] inline double sin_near(double x) noexcept {
    const double square = x * x;
    return x * poly(square, std::array{1.0 / 355687428096000.0, -1.0 / 1307674368000.0,
                                       1.0 / 6227020800.0, -1.0 / 39916800.0, 1.0 / 362880.0,
                                       -1.0 / 5040.0, 1.0 / 120.0, -1.0 / 6.0, 1.0});
}

[[nodiscard]] inline double cos_near(double x) noexcept {
    const double square = x * x;
    return poly(square, std::array{-1.0 / 6402373705728000.0, 1.0 / 20922789888000.0,
                                   -1.0 / 87178291200.0, 1.0 / 479001600.0, -1.0 / 3628800.0,
                                   1.0 / 40320.0, -1.0 / 720.0, 1.0 / 24.0, -1.0 / 2.0, 1.0});
}

// atan on [-tan(π/12), tan(π/12)].
[[nodiscard]] inline double atan_near(double x) noexcept {
    const double square = x * x;
    return x * poly(square, std::array{-1.0 / 23.0, 1.0 / 21.0, -1.0 / 19.0, 1.0 / 17.0,
                                       -1.0 / 15.0, 1.0 / 13.0, -1.0 / 11.0, 1.0 / 9.0, -1.0 / 7.0,
                                       1.0 / 5.0, -1.0 / 3.0, 1.0});
}

} // namespace detail

[[nodiscard]] inline double sin(double radians) noexcept {
    const auto [rest, quarter] = detail::reduce(radians);
    switch (quarter) {
    case 0:
        return detail::sin_near(rest);
    case 1:
        return detail::cos_near(rest);
    case 2:
        return -detail::sin_near(rest);
    default:
        return -detail::cos_near(rest);
    }
}

[[nodiscard]] inline double cos(double radians) noexcept {
    const auto [rest, quarter] = detail::reduce(radians);
    switch (quarter) {
    case 0:
        return detail::cos_near(rest);
    case 1:
        return -detail::sin_near(rest);
    case 2:
        return -detail::cos_near(rest);
    default:
        return detail::sin_near(rest);
    }
}

[[nodiscard]] inline double tan(double radians) noexcept {
    const auto [rest, quarter] = detail::reduce(radians);
    const double s = detail::sin_near(rest);
    const double c = detail::cos_near(rest);
    return (quarter % 2 == 0) ? s / c : -c / s;
}

[[nodiscard]] inline double atan(double x) noexcept {
    constexpr double tan_twelfth = 0.2679491924311227; // tan(π/12)
    constexpr double sqrt3 = std::numbers::sqrt3;
    const bool negative = x < 0.0;
    double t = negative ? -x : x;
    double offset = 0.0;
    bool inverted = false;
    if (t > 1.0) {
        t = 1.0 / t;
        inverted = true;
    }
    if (t > tan_twelfth) {
        // atan(t) = π/6 + atan((√3·t − 1) / (t + √3)).
        t = ((sqrt3 * t) - 1.0) / (t + sqrt3);
        offset = std::numbers::pi / 6.0;
    }
    double angle = offset + detail::atan_near(t);
    if (inverted) {
        angle = (std::numbers::pi / 2.0) - angle;
    }
    return negative ? -angle : angle;
}

[[nodiscard]] inline double atan2(double y, double x) noexcept {
    if (x > 0.0) {
        return atan(y / x);
    }
    if (x < 0.0) {
        return y < 0.0 ? atan(y / x) - std::numbers::pi : atan(y / x) + std::numbers::pi;
    }
    if (y > 0.0) {
        return std::numbers::pi / 2.0;
    }
    return y < 0.0 ? -std::numbers::pi / 2.0 : 0.0;
}

[[nodiscard]] inline double exp(double x) noexcept {
    // ln 2 in two parts, as π/2 above.
    constexpr double ln2_high = 6.93147180369123816490e-01; // NOLINT(modernize-use-std-numbers)
    constexpr double ln2_low = 1.90821492927058770002e-10;
    if (x > 709.0) {
        return HUGE_VAL;
    }
    if (x < -745.0) {
        return 0.0;
    }
    const double halvings = std::nearbyint(x * std::numbers::log2e);
    const double rest = (x - (halvings * ln2_high)) - (halvings * ln2_low);
    const double series = detail::poly(
        rest, std::array{1.0 / 6227020800.0, 1.0 / 479001600.0, 1.0 / 39916800.0, 1.0 / 3628800.0,
                         1.0 / 362880.0, 1.0 / 40320.0, 1.0 / 5040.0, 1.0 / 720.0, 1.0 / 120.0,
                         1.0 / 24.0, 1.0 / 6.0, 0.5, 1.0, 1.0});
    return std::ldexp(series, static_cast<int>(halvings));
}

// Natural log of a positive number; NaN for less.
[[nodiscard]] inline double log(double x) noexcept {
    if (!(x > 0.0)) {
        return x == 0.0 ? -HUGE_VAL : std::numeric_limits<double>::quiet_NaN();
    }
    if (x == HUGE_VAL) {
        return x;
    }
    int exponent = 0;
    double mantissa = std::frexp(x, &exponent); // [0.5, 1)
    if (mantissa < std::numbers::sqrt2 / 2.0) {
        mantissa *= 2.0;
        --exponent;
    }
    // log(m) = 2·atanh(s), s = (m − 1)/(m + 1), |s| < 0.172.
    const double s = (mantissa - 1.0) / (mantissa + 1.0);
    const double square = s * s;
    const double series = detail::poly(
        square, std::array{1.0 / 21.0, 1.0 / 19.0, 1.0 / 17.0, 1.0 / 15.0, 1.0 / 13.0, 1.0 / 11.0,
                           1.0 / 9.0, 1.0 / 7.0, 1.0 / 5.0, 1.0 / 3.0, 1.0});
    return (2.0 * s * series) + (static_cast<double>(exponent) * std::numbers::ln2);
}

// A positive base to any power.
[[nodiscard]] inline double pow(double base, double power) noexcept {
    return exp(power * log(base));
}

} // namespace hog::globe::maths
