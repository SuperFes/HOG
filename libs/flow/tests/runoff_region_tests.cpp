#include "block.hpp"

#include <hog/flow/region.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <algorithm>
#include <cstdint>

using hog::flow::testing::Block;
using hog::flow::testing::floor_above_sea;

namespace {

// Rock up to this many layers over the floor: a bowl, lowest on its +i side.
int bowl(std::uint32_t i, std::uint32_t j) {
    const double di = static_cast<double>(i) - 11.5;
    const double dj = static_cast<double>(j) - 11.5;
    return static_cast<int>(std::lround((0.06 * ((di * di) + (dj * dj))) - (0.25 * di)));
}

Block bowl_block() {
    return Block{floor_above_sea(100.0), false,
                 [](std::uint32_t i, std::uint32_t j, int layer) { return layer >= bowl(i, j); }};
}

} // namespace

TEST_CASE("rain run off the voxels fills a hollow with water voxels to its rim",
          "[flow][runoff]") {
    Block block = bowl_block();
    block.region().settle_water({.rain = [](hog::DVec3) { return 2.0; },
                                 .river = {},
                                 .springs = {},
                                 .evaporation = 0.5});
    // Water lies over the middle, up to the lowest edge column's ground and no higher.
    int rim = 1'000;
    for (std::uint32_t j = 0; j < Block::side; ++j) {
        rim = std::min(rim, bowl(Block::side - 1, j));
    }
    const std::uint32_t middle = 12;
    REQUIRE(bowl(middle, middle) + 2 < rim);
    for (int layer = bowl(middle, middle); layer < rim - 1; ++layer) {
        CHECK(block.amount(middle, middle, layer) > 0);
    }
    for (int layer = rim + 1; layer < rim + 4; ++layer) {
        CHECK(block.amount(middle, middle, layer) == 0);
    }
}

TEST_CASE("without rain the voxels stay dry", "[flow][runoff]") {
    Block block = bowl_block();
    block.region().settle_water({.rain = [](hog::DVec3) { return 0.0; },
                                 .river = {},
                                 .springs = {},
                                 .evaporation = 0.5});
    for (int layer = 0; layer < 16; ++layer) {
        CHECK(block.amount(12, 12, layer) == 0);
    }
}

TEST_CASE("a river let in at the held ground's edge runs down its valley in water voxels",
          "[flow][runoff]") {
    // A valley along +i, its floor down the middle, falling a layer every four columns.
    const auto valley = [](std::uint32_t i, std::uint32_t j) {
        return 10 - static_cast<int>(i / 4) + (std::abs(static_cast<int>(j) - 12) / 2);
    };
    Block block{floor_above_sea(100.0), false,
                [&](std::uint32_t i, std::uint32_t j, int layer) { return layer >= valley(i, j); }};
    block.region().settle_water({.rain = [](hog::DVec3) { return 0.0; },
                                 .river = hog::flow::Region::River{.discharge = 3.0e7, .side = 2},
                                 .springs = {},
                                 .evaporation = 0.5});
    // Down the floor, three columns wide, it runs in one of them; up the sides, none.
    for (std::uint32_t i = 2; i < Block::side - 2; i += 4) {
        std::int64_t floor = 0;
        for (std::uint32_t j = 11; j <= 13; ++j) {
            floor = std::max(floor, block.amount(i, j, valley(i, j)));
        }
        CHECK(floor > 0);
        CHECK(block.amount(i, 3, valley(i, 3)) == 0);
    }
}

TEST_CASE("a spring wells up into a hollow and fills it", "[flow][runoff]") {
    Block block = bowl_block();
    const hog::globe::Site middle = block.at(12, 12, 0);
    block.region().settle_water(
        {.rain = [](hog::DVec3) { return 0.0; },
         .river = {},
         .springs = {{.face = middle.face, .i = middle.i, .j = middle.j, .discharge = 1.0e5}},
         .evaporation = 0.5});
    CHECK(block.amount(12, 12, bowl(12, 12)) > 0);
}
