#include <hog/grid/water_bodies.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <vector>

using Catch::Approx;
using hog::DVec3;
using hog::grid::Lake;
using hog::grid::River;
using hog::grid::Sea;
using hog::grid::Season;
using hog::grid::WaterBodies;

namespace {

// East a hundred metres, then north two hundred, falling and gathering water as it goes.
River bent() {
    return River{
        .course = {{.place = DVec3{0.0, 0.0, 0.0}, .level = 100.0, .flow = 10.0, .rise = 0.5},
                   {.place = DVec3{100.0, 0.0, 0.0}, .level = 90.0, .flow = 20.0, .rise = 1.0},
                   {.place = DVec3{100.0, 200.0, 0.0}, .level = 50.0, .flow = 40.0, .rise = 3.0}},
        .peak = 0.3};
}

WaterBodies some() {
    WaterBodies table;
    table.bodies.emplace(1, Sea{.level = 28'800.0});
    table.bodies.emplace(7, Lake{.level = 29'100.5, .season = Season{.rise = 1.5, .peak = 0.25}});
    table.bodies.emplace(300'000, bent());
    return table;
}

} // namespace

TEST_CASE("a river's level and flow change evenly along its course", "[grid]") {
    const River river = bent();

    const River::Along first = river.along(DVec3{50.0, 5.0, 0.0});
    CHECK(first.level == Approx(95.0));
    CHECK(first.flow == Approx(15.0));
    CHECK(first.rise == Approx(0.75));
    CHECK(first.from_head == Approx(50.0));
    CHECK(first.off == Approx(5.0));
    CHECK(first.downstream.x == Approx(1.0));

    const River::Along second = river.along(DVec3{100.0, 100.0, 3.0});
    CHECK(second.level == Approx(70.0));
    CHECK(second.flow == Approx(30.0));
    CHECK(second.rise == Approx(2.0));
    CHECK(second.from_head == Approx(200.0));
    CHECK(second.off == Approx(3.0));
    CHECK(second.downstream.y == Approx(1.0));

    const River::Along bend = river.along(DVec3{100.0, 0.0, 0.0});
    CHECK(bend.level == Approx(90.0));
    CHECK(bend.flow == Approx(20.0));
    CHECK(bend.off == Approx(0.0));
}

TEST_CASE("past a river's ends it reads as at them", "[grid]") {
    const River river = bent();
    const River::Along above = river.along(DVec3{-30.0, 0.0, 0.0});
    CHECK(above.level == Approx(100.0));
    CHECK(above.from_head == Approx(0.0));
    CHECK(above.off == Approx(30.0));
    const River::Along below = river.along(DVec3{100.0, 260.0, 0.0});
    CHECK(below.level == Approx(50.0));
    CHECK(below.flow == Approx(40.0));
    CHECK(below.from_head == Approx(300.0));
    CHECK(below.off == Approx(60.0));
}

TEST_CASE("water bodies come back from their bytes", "[grid]") {
    const WaterBodies table = some();
    const auto back = WaterBodies::decode(table.encode());
    REQUIRE(back);
    CHECK(*back == table);
    CHECK(back->encode() == table.encode());

    const auto empty = WaterBodies::decode(WaterBodies{}.encode());
    REQUIRE(empty);
    CHECK(empty->bodies.empty());
}

TEST_CASE("broken water body bytes are refused", "[grid]") {
    const std::vector<std::byte> bytes = some().encode();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CHECK_FALSE(WaterBodies::decode(std::span{bytes}.first(length)));
    }
    std::vector<std::byte> format = bytes;
    format[0] = std::byte{99};
    CHECK_FALSE(WaterBodies::decode(format));

    WaterBodies two;
    two.bodies.emplace(1, Sea{.level = 1.0});
    two.bodies.emplace(2, Sea{.level = 2.0});
    // Format, count, then each: id, kind and a level.
    const std::vector<std::byte> seas = two.encode();
    REQUIRE(WaterBodies::decode(seas));
    std::vector<std::byte> twice = seas;
    twice[12] = std::byte{1};
    CHECK_FALSE(WaterBodies::decode(twice));
    std::vector<std::byte> none = seas;
    none[2] = std::byte{0};
    CHECK_FALSE(WaterBodies::decode(none));
    std::vector<std::byte> kind = seas;
    kind[3] = std::byte{3};
    CHECK_FALSE(WaterBodies::decode(kind));

    WaterBodies spring;
    spring.bodies.emplace(4, River{.course = {River::Point{}}, .peak = 0.0});
    CHECK_FALSE(WaterBodies::decode(spring.encode()));
}
