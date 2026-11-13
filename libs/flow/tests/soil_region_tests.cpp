#include "block.hpp"

#include <hog/flow/region.hpp>
#include <hog/globe/voxel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

using hog::flow::testing::Block;
using hog::flow::testing::floor_above_sea;
using hog::globe::Material;

namespace {

// Rock six layers over the floor, level.
Block level_block(Material rock = Material::Basalt) {
    return Block{floor_above_sea(100.0), false,
                 [](std::uint32_t, std::uint32_t, int layer) { return layer >= 6; }, rock};
}

hog::flow::Region::ClimateAt everywhere(hog::globe::Climate climate) {
    return [climate](hog::DVec3, double) { return climate; };
}

} // namespace

TEST_CASE("weathered rock turns to topsoil, clay and rocky dirt in its voxels, down from the top",
          "[flow][soil]") {
    Block block = level_block();
    const hog::flow::Stepped stepped = block.region().weather_ground(everywhere({}), 3.0);
    CHECK(stepped.changed > 0);
    // Half-metre voxels under a 3.6 m blanket on basalt: 0.54 m topsoil, clay to 1.8 m.
    for (std::uint32_t i = 2; i < Block::side - 2; i += 5) {
        CHECK(block.voxel(i, 7, 5).solid == Material::Soil);
        CHECK(block.voxel(i, 7, 3).solid == Material::Clay);
        CHECK(block.voxel(i, 7, 0).solid == Material::Gravel);
        CHECK(block.voxel(i, 7, -6).solid == Material::Basalt);
        // The ground keeps its shape.
        CHECK(block.solid(i, 7, 5) == hog::globe::Voxel::full);
        CHECK(block.solid(i, 7, 6) == 0);
    }
}

TEST_CASE("frozen ground only breaks, and ground already loose is left", "[flow][soil]") {
    Block frozen = level_block();
    static_cast<void>(frozen.region().weather_ground(everywhere({.kelvin = 250.0}), 3.0));
    CHECK(frozen.voxel(9, 9, 5).solid == Material::Gravel);
    Block sand = level_block(Material::Sand);
    CHECK(sand.region().weather_ground(everywhere({}), 3.0).changed == 0);
    CHECK(sand.voxel(9, 9, 5).solid == Material::Sand);
}

namespace {

// Rock up to this many layers over the floor: a bowl, lowest in the middle.
int bowl(std::uint32_t i, std::uint32_t j) {
    const double di = static_cast<double>(i) - 11.5;
    const double dj = static_cast<double>(j) - 11.5;
    return static_cast<int>(std::lround(0.06 * ((di * di) + (dj * dj))));
}

} // namespace

TEST_CASE("rain washes the weathered ground down a bowl's sides", "[flow][soil]") {
    Block block{floor_above_sea(100.0), false,
                [](std::uint32_t i, std::uint32_t j, int layer) { return layer >= bowl(i, j); }};
    static_cast<void>(block.region().weather_ground(everywhere({}), 3.0));
    const hog::flow::Stepped stepped = block.region().wash(
        {.rain = [](hog::DVec3) { return 0.3; }, .river = {}, .springs = {}, .evaporation = 1.0},
        2'000.0);
    CHECK(stepped.washed > 0);
    // What the water dropped lies mostly in the bowl's lower half.
    std::int64_t low = 0;
    std::int64_t high = 0;
    for (std::uint32_t i = 0; i < Block::side; ++i) {
        for (std::uint32_t j = 0; j < Block::side; ++j) {
            for (int layer = 0; layer < 12; ++layer) {
                (bowl(i, j) <= 5 ? low : high) += block.voxel(i, j, layer).grain_amount;
            }
        }
    }
    CHECK(low > 0);
    CHECK(high < low);
}

TEST_CASE("what creeps off a step of weathered ground is its topsoil", "[flow][soil]") {
    Block block{floor_above_sea(100.0), false, [](std::uint32_t i, std::uint32_t, int layer) {
                    return layer >= (i < 12 ? 6 : 3);
                }};
    static_cast<void>(block.region().weather_ground(everywhere({}), 3.0));
    std::int64_t moved = 0;
    for (int step = 0; step < 10; ++step) {
        moved += block.region().age_cover(100.0).crept;
    }
    CHECK(moved > 0);
    const hog::globe::Voxel foot = block.voxel(12, 12, 3);
    REQUIRE(foot.grain_amount > 0);
    CHECK(foot.grain == Material::Soil);
}
