#include <hog/core/hash.hpp>
#include <hog/globe/maths.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace maths = hog::globe::maths;

namespace {

// Within a few ulps of libm's, which is correctly rounded or close to it.
bool near(double ours, double theirs) {
    return std::abs(ours - theirs) <= 4e-16 * std::max(1.0, std::abs(theirs));
}

} // namespace

TEST_CASE("the portable functions agree with libm to a few ulps", "[globe][maths]") {
    for (int k = -4000; k <= 4000; ++k) {
        const double x = k * 0.0123456789;
        CHECK(near(maths::sin(x), std::sin(x)));
        CHECK(near(maths::cos(x), std::cos(x)));
        CHECK(near(maths::atan(x), std::atan(x)));
        CHECK(near(maths::atan2(x, 1.7 - (0.001 * k)), std::atan2(x, 1.7 - (0.001 * k))));
        // tan magnifies the angle's own rounding by 1 + tan².
        const double tangent = std::tan(x);
        CHECK(std::abs(maths::tan(x) - tangent) <=
              (4e-16 * std::abs(tangent)) +
                  (2.2e-16 * std::max(1.0, std::abs(x)) * (1.0 + (tangent * tangent))));
        CHECK(std::abs((maths::exp(x * 0.1) / std::exp(x * 0.1)) - 1.0) < 1e-15);
        const double positive = (std::abs(x) * 37.0) + 1e-300;
        CHECK(near(maths::log(positive), std::log(positive)));
    }
    CHECK(maths::atan2(0.0, -1.0) == std::numbers::pi);
    CHECK(maths::atan2(-1.0, 0.0) == -std::numbers::pi / 2.0);
    CHECK(maths::exp(0.0) == 1.0);
    CHECK(maths::log(1.0) == 0.0);
    CHECK(std::isnan(maths::log(-1.0)));
    CHECK(std::abs(maths::pow(2.0, 0.5) - std::numbers::sqrt2) < 1e-15);
}

TEST_CASE("the portable functions give the same bits forever", "[globe][maths]") {
    std::uint64_t digest = 0;
    for (int k = -500; k <= 500; ++k) {
        const double x = k * 0.0731;
        for (const double value :
             {maths::sin(x), maths::cos(x), maths::tan(x), maths::atan(x), maths::atan2(x, 0.3),
              maths::exp(x * 0.5), maths::log(std::abs(x) + 0.01)}) {
            digest = hog::hash_combine(digest, std::bit_cast<std::uint64_t>(value));
        }
    }
    // If this moves, worlds regenerate differently from before, or on another platform.
    CHECK(digest == 0x4704'822f'800e'808cULL);
}
