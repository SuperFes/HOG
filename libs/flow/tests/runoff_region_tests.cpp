#include "block.hpp"

#include <hog/flow/region.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
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
                                 .inflow = {},
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
                                 .inflow = {},
                                 .evaporation = 0.5});
    for (int layer = 0; layer < 16; ++layer) {
        CHECK(block.amount(12, 12, layer) == 0);
    }
}
