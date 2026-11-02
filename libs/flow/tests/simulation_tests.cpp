#include "block.hpp"

#include <hog/flow/simulation.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

using hog::flow::Simulation;
using hog::flow::testing::Block;
using hog::flow::testing::earth;
using hog::flow::testing::floor_above_sea;
using hog::flow::testing::full;
using hog::flow::testing::tick;
using hog::globe::VoxelStore;
using hog::globe::World;

namespace {

// Steps until every brick is let go, or `most` steps; how many it took, and the most bricks
// held at once.
struct Ran {
    int steps = 0;
    std::size_t most_held = 0;
};

Ran run(Simulation& simulation, int most) {
    Ran ran;
    for (; ran.steps < most; ++ran.steps) {
        const Simulation::Progress progress = simulation.step(tick);
        ran.most_held = std::max(ran.most_held, progress.held);
        if (progress.held == 0) {
            ++ran.steps;
            break;
        }
    }
    return ran;
}

// Open within a rim of rock a column wide, so what's poured stays on the block.
bool rimmed(std::uint32_t i, std::uint32_t j, int /*layer*/) {
    return i > 0 && i + 1 < Block::side && j > 0 && j + 1 < Block::side;
}

} // namespace

TEST_CASE("an edit wakes the flow about it, which sleeps again once at rest",
          "[flow][simulation]") {
    Block block{floor_above_sea(100.0), false, rimmed};
    block.pour(12, 12, 5, full);
    block.region().flush();
    Simulation simulation{block.world()};
    CHECK(simulation.step(tick).held == 0);
    simulation.touched(hog::globe::brick_of(block.at(12, 12, 5)));
    const Ran ran = run(simulation, 10 * Simulation::rest);
    CHECK(ran.steps < 10 * Simulation::rest);
    CHECK(ran.most_held >= 7);
    CHECK(block.world().at(block.at(12, 12, 5)).liquid_amount == 0);
    CHECK(block.world().at(block.at(12, 12, 0)).liquid_amount > 0);
    CHECK(block.world().at(block.at(13, 12, 0)).liquid_amount > 0);
}

TEST_CASE("liquid takes up the bricks it reaches and leaves them once still",
          "[flow][simulation]") {
    Block block{floor_above_sea(100.0), false, rimmed};
    for (int layer = 0; layer < 10; ++layer) {
        block.pour(12, 12, layer, full);
    }
    block.region().flush();
    Simulation simulation{block.world()};
    simulation.touched(hog::globe::brick_of(block.at(12, 12, 0)));
    const Ran ran = run(simulation, 100 * Simulation::rest);
    CHECK(ran.steps < 100 * Simulation::rest);
    // All nine bricks across the floor at least.
    CHECK(ran.most_held >= 9);
    std::int64_t total = 0;
    for (std::uint32_t j = 1; j + 1 < Block::side; ++j) {
        for (std::uint32_t i = 1; i + 1 < Block::side; ++i) {
            const std::int64_t amount = block.world().at(block.at(i, j, 0)).liquid_amount;
            CHECK(amount > 0);
            total += amount;
        }
    }
    // All but the few units that soaked into the rock.
    CHECK(total >= (10 * full) - 16);
    CHECK(total <= 10 * full);
    // The water left its mark where it stood and ran, and none where it never came.
    const hog::globe::Site middle = block.at(12, 12, 0);
    const hog::globe::Site rim = block.at(0, 0, 0);
    CHECK(simulation.moisture(middle.face, middle.i, middle.j).stood > 0.0);
    CHECK(simulation.moisture(middle.face, middle.i, middle.j).ran > 0.0);
    CHECK(simulation.moisture(rim.face, rim.i, rim.j).stood == 0.0);
}

TEST_CASE("the world as generated is left as it is", "[flow][simulation]") {
    const hog::globe::Generator& generator = earth();
    VoxelStore store{generator.layers()};
    World world{generator, store};
    Simulation simulation{world};
    const hog::globe::Cell cell = hog::globe::cell_at_index(7, 2'654'435'761ULL % 98'304);
    const double ground =
        hog::globe::ground_below(generator, hog::globe::center(cell), 0.5).value_or(0.0);
    const hog::globe::Cell column =
        hog::globe::cell_under(hog::globe::center(cell), generator.layers().level);
    simulation.touched(hog::globe::brick_of(
        {.face = column.face,
         .i = column.i,
         .j = column.j,
         .layer = static_cast<std::uint32_t>(generator.layers().layer_at(ground))}));
    const Ran ran = run(simulation, 2 * Simulation::rest);
    CHECK(ran.steps == Simulation::rest + 1);
    CHECK(ran.most_held == 7);
    CHECK(store.size() == 0);
}
