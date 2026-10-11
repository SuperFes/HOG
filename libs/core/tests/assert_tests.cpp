#include <hog/core/assert.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

int checked_half(int value) {
    HOG_ASSERT(value % 2 == 0, "value must be even");
    return value / 2;
}

} // namespace

TEST_CASE("a passing assertion does nothing", "[core][assert]") {
    const hog::testing::AssertionGuard guard;
    CHECK(checked_half(4) == 2);
}

TEST_CASE("a failing assertion reports expression and message", "[core][assert]") {
    const hog::testing::AssertionGuard guard;
    try {
        checked_half(3);
        FAIL("assertion did not fire");
    } catch (const hog::testing::AssertionFailed& failure) {
        CHECK(failure.expression() == "value % 2 == 0");
        CHECK(failure.message() == "value must be even");
    }
}

TEST_CASE("assertions stay enabled in every build type", "[core][assert]") {
    const hog::testing::AssertionGuard guard;
    CHECK_THROWS_AS(checked_half(1), hog::testing::AssertionFailed);
}

TEST_CASE("the guard restores the previous handler", "[core][assert]") {
    const auto custom = [](const hog::AssertionFailure&) {};
    const auto original = hog::set_assertion_handler(custom);
    {
        const hog::testing::AssertionGuard guard;
    }
    CHECK(hog::set_assertion_handler(original) == +custom);
}
