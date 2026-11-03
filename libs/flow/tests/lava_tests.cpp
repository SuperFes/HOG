#include <hog/flow/lava.hpp>
#include <hog/globe/material.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using hog::flow::cooled;
using hog::flow::is_molten;
using hog::flow::lava_mobility;
using hog::globe::Material;

TEST_CASE("lava thickens as it cools and sets stiff under its melting point", "[flow][lava]") {
    const double melts = hog::globe::properties(Material::Basalt).melts;
    CHECK(lava_mobility(Material::Basalt, melts + 100.0) == 1.0);
    CHECK(lava_mobility(Material::Basalt, melts) > lava_mobility(Material::Basalt, melts - 100.0));
    CHECK(lava_mobility(Material::Basalt, melts - 100.0) > 0.0);
    CHECK(lava_mobility(Material::Basalt, melts - 300.0) == 0.0);
    CHECK(lava_mobility(Material::Air, 2'000.0) == 0.0);
}

TEST_CASE("rock is molten from its melting point up", "[flow][lava]") {
    const double melts = hog::globe::properties(Material::Basalt).melts;
    CHECK(is_molten(Material::Basalt, melts));
    CHECK_FALSE(is_molten(Material::Basalt, melts - 1.0));
    CHECK_FALSE(is_molten(Material::Air, 2'000.0));
}

TEST_CASE("open lava cools towards the air, thin lava faster, buried lava not at all",
          "[flow][lava]") {
    const double hot = 1'500.0;
    const double after_hour = cooled(Material::Basalt, hot, 280.0, 1.0, 0.5, 3'600.0);
    CHECK(after_hour < hot);
    CHECK(after_hour > 280.0);
    CHECK(cooled(Material::Basalt, hot, 280.0, 1.0, 0.1, 3'600.0) < after_hour);
    CHECK(cooled(Material::Basalt, hot, 280.0, 0.0, 0.5, 3'600.0) == hot);
    CHECK(cooled(Material::Basalt, hot, 280.0, 1.0, 0.5, 0.0) == Catch::Approx(hot));
    // Never below the air.
    CHECK(cooled(Material::Basalt, hot, 280.0, 1.0, 0.1, 1e9) >= 280.0);
}

#include "block.hpp"

using hog::flow::Stepped;
using hog::flow::testing::Block;
using hog::flow::testing::floor_above_sea;
using hog::flow::testing::full;
using hog::flow::testing::tick;
using hog::globe::kelvin;

TEST_CASE("hot lava spreads over the floor, keeping its heat and all it was", "[flow][lava]") {
    Block block{floor_above_sea(100.0)};
    for (int layer = 0; layer < 4; ++layer) {
        block.pour_lava(12, 12, layer, full, 1'700.0);
    }
    for (int step = 0; step < 4; ++step) {
        block.region().step(tick);
    }
    CHECK(block.region().liquid() == 4 * full);
    CHECK(block.amount(13, 12, 0) > 0);
    const hog::globe::Voxel edge = block.voxel(13, 12, 0);
    CHECK(edge.liquid == Material::Basalt);
    CHECK(kelvin(edge.temperature) > 1'650.0);
}

TEST_CASE("stiff lava runs slower than hot lava", "[flow][lava]") {
    const double melts = hog::globe::properties(Material::Basalt).melts;
    const auto spread = [&](double kelvin_poured) {
        Block block{floor_above_sea(100.0)};
        for (int layer = 0; layer < 4; ++layer) {
            block.pour_lava(12, 12, layer, full, kelvin_poured);
        }
        block.region().step(tick);
        return block.amount(13, 12, 0);
    };
    CHECK(spread(melts - 100.0) > 0);
    CHECK(spread(melts - 100.0) < spread(melts + 100.0));
}

TEST_CASE("lava cools in the open and sets into rock, reshaping the ground about it",
          "[flow][lava]") {
    Block block{floor_above_sea(100.0)};
    const double melts = hog::globe::properties(Material::Basalt).melts;
    block.pour_lava(12, 12, 0, full, melts + 100.0);
    const double thickness = block.store().layers().thickness;
    block.region().step(500.0);
    const hog::globe::Voxel warm = block.voxel(12, 12, 0);
    CHECK(warm.liquid == Material::Basalt);
    CHECK(kelvin(warm.temperature) < melts + 100.0);
    Stepped stepped;
    for (int hour = 0; hour < 12 && block.voxel(12, 12, 0).liquid != Material::Air; ++hour) {
        stepped = block.region().step(3'600.0);
    }
    const hog::globe::Voxel set = block.voxel(12, 12, 0);
    CHECK(set.liquid == Material::Air);
    CHECK(set.liquid_amount == 0);
    CHECK(set.solid == Material::Basalt);
    CHECK(set.distance < 0);
    // The air over it is now a half-voxel off the new surface.
    CHECK(block.voxel(12, 12, 1).distance <= static_cast<int>(thickness * 500.0));
    CHECK(block.region().liquid() == 0);
}

TEST_CASE("buried lava keeps its heat, and lava is not standing water", "[flow][lava]") {
    Block block{floor_above_sea(100.0), false, [](std::uint32_t i, std::uint32_t j, int layer) {
                    // A one-voxel pocket roofed over.
                    return layer == 0 && i == 12 && j == 12;
                }};
    const double melts = hog::globe::properties(Material::Basalt).melts;
    block.pour_lava(12, 12, 0, full, melts + 100.0);
    const Stepped stepped = block.region().step(3'000.0);
    CHECK(kelvin(block.voxel(12, 12, 0).temperature) == Catch::Approx(melts + 100.0).margin(0.2));
    CHECK(stepped.wet.empty());
}
