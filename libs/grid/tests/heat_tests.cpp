#include <hog/grid/heat.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <vector>

using Catch::Approx;
using hog::DVec3;
using hog::grid::Heat;

namespace {

Heat two() {
    return Heat{.sources = {{.place = DVec3{0.0}, .radius = 10.0, .kelvin = 800.0},
                            {.place = DVec3{12.0, 0.0, 0.0}, .radius = 4.0, .kelvin = 100.0}}};
}

} // namespace

TEST_CASE("heat falls smoothly from a source's middle to none at its reach", "[grid]") {
    const Heat heat = two();
    CHECK(heat.over(DVec3{0.0}) == Approx(800.0));
    CHECK(heat.over(DVec3{0.0, 5.0, 0.0}) == Approx(800.0 * 0.75 * 0.75));
    CHECK(heat.over(DVec3{0.0, 0.0, -10.0}) == 0.0);
    CHECK(heat.over(DVec3{0.0, 50.0, 0.0}) == 0.0);
    CHECK(Heat{}.over(DVec3{1.0, 2.0, 3.0}) == 0.0);
}

TEST_CASE("heat from sources that overlap adds up", "[grid]") {
    const Heat heat = two();
    const DVec3 between{9.0, 0.0, 0.0};
    CHECK(heat.over(between) == Approx((800.0 * 0.19 * 0.19) + (100.0 * 0.4375 * 0.4375)));
}

TEST_CASE("heat comes back from its bytes", "[grid]") {
    const auto back = Heat::decode(two().encode());
    REQUIRE(back);
    CHECK(*back == two());
    const auto none = Heat::decode(Heat{}.encode());
    REQUIRE(none);
    CHECK(none->sources.empty());
}

TEST_CASE("broken heat bytes are refused", "[grid]") {
    const std::vector<std::byte> bytes = two().encode();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CHECK_FALSE(Heat::decode(std::span{bytes}.first(length)));
    }
    std::vector<std::byte> format = bytes;
    format[0] = std::byte{99};
    CHECK_FALSE(Heat::decode(format));
    const Heat nowhere{.sources = {{.place = DVec3{0.0}, .radius = 0.0, .kelvin = 5.0}}};
    CHECK_FALSE(Heat::decode(nowhere.encode()));
}
