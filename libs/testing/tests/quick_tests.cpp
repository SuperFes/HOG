#include <hog/testing/quick.hpp>

#include <catch2/catch_test_macros.hpp>

using hog::testing::quick_from;

TEST_CASE("the longest tests run small only when asked to", "[testing]") {
    CHECK_FALSE(quick_from(nullptr));
    CHECK_FALSE(quick_from(""));
    CHECK_FALSE(quick_from("0"));
    CHECK(quick_from("1"));
    CHECK(quick_from("yes"));
}
