#include "block.hpp"

#include <hog/flow/simulation.hpp>
#include <hog/flow/wear.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using hog::flow::Exposure;
using hog::flow::testing::Block;
using hog::flow::testing::floor_above_sea;
using hog::globe::Material;

TEST_CASE("only exposed, wet and freezing matter wears fast, and only what can wear", "[flow][wear]") {
    const Exposure dry{.open = 1.0};
    const Exposure wet{.open = 1.0, .damp = 1.0};
    const Exposure freezing{.open = 1.0, .damp = 1.0, .freeze = 1.0};
    CHECK(hog::flow::wearing_rate(Material::Sandstone, {}) == 0.0); // buried
    CHECK(hog::flow::wearing_rate(Material::Sandstone, wet) >
          hog::flow::wearing_rate(Material::Sandstone, dry));
    CHECK(hog::flow::wearing_rate(Material::Sandstone, freezing) >
          hog::flow::wearing_rate(Material::Sandstone, wet));
    // Frost needs water to work on.
    CHECK(hog::flow::wearing_rate(Material::Sandstone, {.open = 1.0, .freeze = 1.0}) ==
          hog::flow::wearing_rate(Material::Sandstone, dry));
    // Iron rusts in years where granite weathers in ages, and rust wants water.
    CHECK(hog::flow::wearing_rate(Material::Iron, dry) >
          500.0 * hog::flow::wearing_rate(Material::Granite, dry));
    CHECK(hog::flow::wearing_rate(Material::Iron, wet) >
          10.0 * hog::flow::wearing_rate(Material::Iron, dry));
    CHECK(hog::flow::wearing_rate(Material::Sand, wet) == 0.0);
    CHECK(hog::flow::wearing_rate(Material::Air, wet) == 0.0);
}

TEST_CASE("small losses add up in the long run, and never wrap", "[flow][wear]") {
    // A tenth of a step each time is a step in ten, on average.
    int lost = 0;
    for (int k = 0; k < 1'000; ++k) {
        lost += 255 - hog::flow::worn(255, 0.1 / 255.0, 1.0, (k + 0.5) / 1'000.0);
    }
    CHECK(lost == 100);
    CHECK(hog::flow::worn(3, 1.0, 1.0, 0.5) == 0);
}

TEST_CASE("a voxel's softness is its own and spans a range", "[flow][wear]") {
    double least = 10.0;
    double most = 0.0;
    for (std::uint64_t key = 0; key < 1'000; ++key) {
        const double soft = hog::flow::softness(key);
        CHECK(soft == hog::flow::softness(key));
        least = std::min(least, soft);
        most = std::max(most, soft);
    }
    CHECK(least >= 0.2);
    CHECK(most <= 2.6);
    CHECK(most > 4.0 * least);
}

TEST_CASE("weather wears the open surface of rock and leaves what is buried", "[flow][wear]") {
    // Ground four layers thick raised over the floor.
    const auto raised = [](std::uint32_t, std::uint32_t, int layer) { return layer >= 4; };
    Block block{floor_above_sea(100.0), false, raised, Material::Sandstone};
    // The floor's top layer is the one under the first open layer.
    constexpr int top = 3;
    block.region().disturb(block.bricks()[13], 3e11);
    for (int step = 0; step < 20; ++step) {
        block.region().step(hog::flow::testing::tick);
    }

    int worn_voxels = 0;
    for (std::uint32_t j = 4; j < 20; ++j) {
        for (std::uint32_t i = 4; i < 20; ++i) {
            worn_voxels += block.region().at(block.at(i, j, top)).condition < 255 ? 1 : 0;
            CHECK(block.region().at(block.at(i, j, 0)).condition == 255);
        }
    }
    CHECK(worn_voxels > 0);
    // Soft spots and sound ones side by side, not an even fade.
    CHECK(worn_voxels < 16 * 16);
}

TEST_CASE("iron left out long enough rusts through", "[flow][wear]") {
    const auto raised = [](std::uint32_t, std::uint32_t, int layer) { return layer >= 4; };
    Block block{floor_above_sea(100.0), false, raised, Material::Iron};
    constexpr int top = 3;
    // Many lifetimes of weather at once.
    block.region().disturb(block.bricks()[13], 1e12);
    for (int step = 0; step < 400; ++step) {
        block.region().step(hog::flow::testing::tick);
    }
    // The skin that was out in the weather is gone; what was under it is still whole.
    CHECK(block.region().at(block.at(11, 11, top)).solid == Material::Air);
    CHECK(block.region().at(block.at(11, 11, 0)).solid == Material::Iron);
}

TEST_CASE("the sweep wears the ground it goes round by the time since it was last worn",
          "[flow][wear]") {
    const auto raised = [](std::uint32_t, std::uint32_t, int layer) { return layer >= 4; };
    Block block{floor_above_sea(100.0), false, raised, Material::Sandstone};
    hog::flow::Simulation simulation{block.world()};
    // A whole round, with a round's time spread over its calls.
    const auto bricks = block.bricks().size();
    for (std::size_t call = 0; call < bricks; ++call) {
        simulation.sweep(1);
        simulation.pass(1e10);
        simulation.step(hog::flow::testing::tick);
    }
    int worn_voxels = 0;
    for (std::uint32_t j = 4; j < 20; ++j) {
        for (std::uint32_t i = 4; i < 20; ++i) {
            worn_voxels += simulation.region().at(block.at(i, j, 3)).condition < 255 ? 1 : 0;
        }
    }
    CHECK(worn_voxels > 0);
}
