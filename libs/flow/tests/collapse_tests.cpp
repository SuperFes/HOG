#include "block.hpp"

#include <hog/flow/simulation.hpp>

#include <catch2/catch_test_macros.hpp>

using hog::flow::testing::Block;
using hog::flow::testing::floor_above_sea;
using hog::globe::Material;

namespace {

// The brick in the middle of the block, which the box of support is taken about.
hog::globe::BrickSite middle(Block& block) {
    return block.bricks()[13];
}

bool solid(Block& block, std::uint32_t i, std::uint32_t j, int layer) {
    return block.region().at(block.at(i, j, layer)).solid != Material::Air;
}

// Steps until nothing is being looked at and nothing flies, for at most `most` steps.
int rest(Block& block, int most) {
    for (int step = 1; step <= most; ++step) {
        block.region().step(hog::flow::testing::tick);
        if (block.region().inspecting() == 0 && block.region().parcels().empty()) {
            return step;
        }
    }
    return most;
}

} // namespace

TEST_CASE("a lump of ground hanging from nothing cracks through and falls", "[flow][collapse]") {
    Block block{floor_above_sea(100.0), false, [](std::uint32_t i, std::uint32_t j, int layer) {
                    const bool slab = i >= 8 && i <= 14 && j >= 8 && j <= 14 && layer >= 3 &&
                                      layer <= 5;
                    return !slab;
                }};
    REQUIRE(solid(block, 11, 11, 4));
    block.region().disturb(middle(block));

    // It sags first: nothing falls in the first second.
    for (int step = 0; step < 20; ++step) {
        block.region().step(hog::flow::testing::tick);
    }
    CHECK(solid(block, 11, 11, 4));
    CHECK(block.region().inspecting() == 1);

    CHECK(rest(block, 1'200) < 1'200);
    // It came down off where it hung, and was built into the floor as a round boulder: the slab's
    // corners and top are gone, and the floor under its middle is rock.
    CHECK_FALSE(solid(block, 8, 8, 4));
    CHECK_FALSE(solid(block, 11, 11, 5));
    CHECK(solid(block, 11, 11, 0));
}

TEST_CASE("ground standing on the ground, or hung within its reach, stays", "[flow][collapse]") {
    Block block{floor_above_sea(100.0), false,
                [](std::uint32_t i, std::uint32_t j, int layer) {
                    const bool pillar = i >= 3 && i <= 4 && j >= 8 && j <= 14 && layer <= 6;
                    const bool slab = i >= 5 && i <= 9 && j >= 8 && j <= 14 && layer >= 5 &&
                                      layer <= 6;
                    return !(pillar || slab);
                },
                Material::Sandstone};
    block.region().disturb(middle(block));
    for (int step = 0; step < 100; ++step) {
        block.region().step(hog::flow::testing::tick);
    }
    CHECK(solid(block, 9, 11, 5));
    CHECK(solid(block, 3, 11, 5));
    CHECK(block.region().inspecting() == 0);
}

TEST_CASE("an overhang longer than the rock can reach breaks off at its tip", "[flow][collapse]") {
    Block block{floor_above_sea(100.0), false,
                [](std::uint32_t i, std::uint32_t j, int layer) {
                    const bool pillar = i >= 3 && i <= 4 && j >= 8 && j <= 14 && layer <= 6;
                    const bool slab = i >= 5 && i <= 22 && j >= 8 && j <= 14 && layer >= 5 &&
                                      layer <= 6;
                    return !(pillar || slab);
                },
                Material::Sandstone};
    block.region().disturb(middle(block));
    REQUIRE(solid(block, 22, 11, 5));
    CHECK(rest(block, 1'200) < 1'200);
    CHECK_FALSE(solid(block, 22, 11, 5));
    CHECK(solid(block, 6, 11, 5));
    CHECK(solid(block, 3, 11, 5));
}

TEST_CASE("the sweep brings down hanging ground nobody is near", "[flow][collapse]") {
    Block block{floor_above_sea(100.0), false, [](std::uint32_t i, std::uint32_t j, int layer) {
                    const bool slab = i >= 8 && i <= 14 && j >= 8 && j <= 14 && layer >= 3 &&
                                      layer <= 5;
                    return !slab;
                }};
    hog::flow::Simulation simulation{block.world()};
    std::int64_t collapsed = 0;
    // A brick a step, going round those changed from the generated ground.
    for (int step = 0; step < 1'500 && collapsed == 0; ++step) {
        if (step < static_cast<int>(block.bricks().size())) {
            simulation.sweep(1);
        }
        collapsed += simulation.step(hog::flow::testing::tick).collapsed;
    }
    CHECK(collapsed > 0);
}
