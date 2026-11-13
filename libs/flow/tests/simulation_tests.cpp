#include "block.hpp"

#include <hog/flow/simulation.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using hog::flow::Simulation;
using hog::flow::testing::Block;
using hog::flow::testing::earth;
using hog::flow::testing::floor_above_sea;
using hog::flow::testing::full;
using hog::flow::testing::tick;
using hog::globe::MadeWorld;
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

// The brick at the ground under a direction.
hog::globe::BrickSite ground_brick(const hog::globe::Generator& generator, hog::DVec3 direction) {
    const double ground = hog::globe::ground_below(generator, direction, 0.5).value_or(0.0);
    const hog::globe::Cell column = hog::globe::cell_under(direction, generator.layers().level);
    return hog::globe::brick_of(
        {.face = column.face,
         .i = column.i,
         .j = column.j,
         .layer = static_cast<std::uint32_t>(generator.layers().layer_at(ground))});
}

// A direction whose ground stands between two heights over the sea's level, the `nth` found.
hog::DVec3 ground_between(const hog::globe::Generator& generator, double low, double high,
                          int nth = 0) {
    const double sea = generator.sea_level().value_or(generator.planet().body.radius);
    for (std::uint64_t k = 0;; ++k) {
        const hog::DVec3 direction =
            hog::globe::center(hog::globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304));
        const double ground =
            hog::globe::ground_below(generator, direction, 0.5).value_or(0.0) - sea;
        if (ground > low && ground < high && nth-- == 0) {
            return direction;
        }
    }
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

TEST_CASE("grain thrown over a cliff keeps the bricks awake while it flies, then lands and sleeps",
          "[flow][simulation]") {
    using hog::globe::Material;
    const auto shape = [](std::uint32_t i, std::uint32_t, int layer) {
        return i >= 12 || layer >= 15;
    };
    Block block{floor_above_sea(100.0), false, shape};
    block.pile(11, 12, 15, Material::Sand, full);
    block.region().flush();
    const auto total = [&] {
        std::int64_t sum = 0;
        for (std::uint32_t i = 0; i < Block::side; ++i) {
            for (std::uint32_t j = 0; j < Block::side; ++j) {
                for (int layer = 0; layer < 40; ++layer) {
                    sum += block.world().at(block.at(i, j, layer)).grain_amount;
                }
            }
        }
        return sum;
    };
    const std::int64_t before = total();
    REQUIRE(before == full);

    Simulation simulation{block.world()};
    simulation.touched(hog::globe::brick_of(block.at(11, 12, 15)));
    int flew = 0;
    int steps = 0;
    for (; steps < 4000; ++steps) {
        const Simulation::Progress progress = simulation.step(tick);
        if (progress.flying > 0) {
            ++flew;
            // Nothing is let go while grain may still land in it.
            CHECK(progress.held > 0);
        }
        if (progress.held == 0) {
            break;
        }
    }
    CHECK(steps < 4000);
    CHECK(flew > 20);
    CHECK(simulation.region().parcels().empty());
    CHECK(total() == before);
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

TEST_CASE("the world as generated is at rest wherever it's touched: seabed, shore, lowland and "
          "mountain",
          "[flow][simulation]") {
    const hog::globe::Generator& generator = earth();
    struct Landform {
        const char* name;
        double low;
        double high;
    };
    for (const Landform& landform :
         {Landform{.name = "seabed", .low = -300.0, .high = -20.0},
          Landform{.name = "shore", .low = -3.0, .high = 3.0},
          Landform{.name = "lowland", .low = 20.0, .high = 300.0},
          Landform{.name = "mountain", .low = 1'500.0, .high = 9'000.0}}) {
        for (int nth = 0; nth < 2; ++nth) {
            INFO(landform.name << " " << nth);
            MadeWorld made{generator};
            World& world = made.world();
            const hog::DVec3 up = ground_between(generator, landform.low, landform.high, nth);
            made.cover(up);
            Simulation simulation{world};
            simulation.touched(ground_brick(generator, up));
            std::int64_t moved = 0;
            std::int64_t eroded = 0;
            std::size_t most_held = 0;
            int steps = 0;
            for (; steps < 3 * Simulation::rest; ++steps) {
                const Simulation::Progress progress = simulation.step(tick);
                moved += std::abs(progress.changed);
                eroded += progress.eroded;
                most_held = std::max(most_held, progress.held);
                if (progress.held == 0) {
                    break;
                }
            }
            // Nothing moves, nothing wears, nothing is written, and the flow neither spreads
            // along the sea nor stays awake.
            CHECK(moved == 0);
            CHECK(eroded == 0);
            CHECK(world.revision() == 0);
            CHECK(most_held <= 7);
            CHECK(steps == Simulation::rest);
        }
    }
}

TEST_CASE("the world as generated is left as it is", "[flow][simulation]") {
    const hog::globe::Generator& generator = earth();
    MadeWorld made{generator};
    World& world = made.world();
    Simulation simulation{world};
    const hog::globe::Cell cell = hog::globe::cell_at_index(7, 2'654'435'761ULL % 98'304);
    const double ground =
        hog::globe::ground_below(generator, hog::globe::center(cell), 0.5).value_or(0.0);
    const hog::globe::Cell column =
        hog::globe::cell_under(hog::globe::center(cell), generator.layers().level);
    made.cover(hog::globe::center(cell));
    simulation.touched(hog::globe::brick_of(
        {.face = column.face,
         .i = column.i,
         .j = column.j,
         .layer = static_cast<std::uint32_t>(generator.layers().layer_at(ground))}));
    const Ran ran = run(simulation, 2 * Simulation::rest);
    CHECK(ran.steps == Simulation::rest + 1);
    CHECK(ran.most_held == 7);
    CHECK(world.revision() == 0);
}

TEST_CASE("rain wets ground the flow does not hold, and a film on it wakes the brick",
          "[flow][simulation][wetting]") {
    const int floor = floor_above_sea(300.0);
    Block block{floor, false, {}, hog::globe::Material::Soil};
    Simulation simulation{block.world()};
    const auto wetted = [&](double rain) {
        const hog::globe::Site at = block.at(12, 12, 0);
        return Simulation::Wetted{
            .column = {.face = at.face, .i = at.i, .j = at.j, .layer = at.layer + 2},
            .sky = {.rain = rain}};
    };
    const hog::globe::Site bed = block.at(12, 12, -1);

    // A light shower soaks in: the world's bed voxel holds water, and nothing wakes.
    std::size_t written = 0;
    for (int k = 0; k < 360; ++k) {
        const std::array columns{wetted(5.0)};
        written += simulation.wet(columns, 10.0).size();
    }
    CHECK(written > 0);
    CHECK(block.world().at(bed).liquid_amount > 100);
    CHECK(simulation.region().size() == 0);

    // A downpour outruns the ground and lies on it, which the flow takes up.
    const std::array columns{wetted(500.0)};
    for (int k = 0; k < 6; ++k) {
        (void)simulation.wet(columns, 10.0);
    }
    CHECK(block.world().at(block.at(12, 12, 0)).liquid_amount > 0);
    CHECK(simulation.region().size() > 0);
}

TEST_CASE("a vent gives lava while it erupts and the flow holds its bricks",
          "[flow][simulation][vent]") {
    Block block{floor_above_sea(100.0), false, rimmed};
    Simulation simulation{block.world()};
    simulation.add_vent({.site = block.at(12, 12, 0),
                         .active_rate = 0.0,
                         .erupting_rate = 1.0,
                         .period = 1'000.0,
                         .eruption = 20.0,
                         .active = 0.0});
    REQUIRE(simulation.vents() == 1);
    for (int step = 0; step < 400; ++step) {
        simulation.step(tick);
    }
    // 20 s at a cubic metre a second is 160 voxels' worth, less what backed up.
    CHECK(simulation.region().size() > 0);
    CHECK(simulation.region().liquid() > 20 * full);
    CHECK(simulation.region().liquid() <= 160 * full);
    // Dormant for the rest of the cycle: nothing more comes.
    const std::int64_t before = simulation.region().liquid();
    simulation.pass(10.0);
    for (int step = 0; step < 100; ++step) {
        simulation.step(tick);
    }
    CHECK(simulation.region().liquid() == before);
}

TEST_CASE("aging a world's ice grows it in the bricks written to the world",
          "[flow][simulation][glacier]") {
    Block block{floor_above_sea(100.0), false, rimmed};
    block.region().flush();
    Simulation simulation{block.world()};
    simulation.set_ice_balance([](const hog::globe::Cell&, double) { return 2.0; });
    const hog::globe::BrickSite brick = hog::globe::brick_of(block.at(12, 12, 0));
    const std::vector<hog::globe::BrickSite> written =
        simulation.age_ice(std::span{&brick, 1}, 20.0);
    CHECK_FALSE(written.empty());
    CHECK(block.world().at(block.at(12, 12, 0)).grain == hog::globe::Material::Ice);
    CHECK(block.world().at(block.at(12, 12, 0)).grain_amount > 0);
}

TEST_CASE("ice laid over a column lies on its ground and is written to the world",
          "[flow][simulation][glacier]") {
    using hog::globe::Material;
    const auto& generator = earth();
    // The highest ground over a coarse sweep of the sphere: land, whatever the seed.
    hog::DVec3 up{0.0, 0.0, 1.0};
    double top = 0.0;
    for (int k = 0; k < 4000; ++k) {
        const double z = 1.0 - (((2.0 * k) + 1.0) / 4000.0);
        const double r = std::sqrt(1.0 - (z * z));
        const hog::DVec3 d{r * std::cos(2.399963 * k), r * std::sin(2.399963 * k), z};
        if (const double ground = generator.column(d, 200.0).ground(); ground > top) {
            top = ground;
            up = d;
        }
    }
    MadeWorld made{generator};
    World& world = made.world();
    made.cover(up);
    Simulation simulation{world};
    const hog::globe::Cell cell = hog::globe::cell_under(up, generator.layers().level);
    const std::vector<Simulation::Laid> laid{
        {.face = cell.face, .i = cell.i, .j = cell.j, .metres = 6.0}};
    const auto written = simulation.lay_ice(laid);
    CHECK_FALSE(written.empty());
    const auto ground =
        generator.column(hog::globe::center(cell), generator.layers().thickness).ground();
    const auto layer = static_cast<std::uint32_t>(generator.layers().layer_at(ground));
    double ice = 0.0;
    for (std::uint32_t up_by = 0; up_by < 12; ++up_by) {
        const auto voxel =
            world.at({.face = cell.face, .i = cell.i, .j = cell.j, .layer = layer + up_by});
        if (voxel.grain == Material::Ice) {
            ice += static_cast<double>(voxel.grain_amount) / static_cast<double>(full) *
                   generator.layers().thickness;
        }
    }
    CHECK(ice > 4.0);
    CHECK(ice <= 6.5);
    CHECK(simulation.region().size() == 0);
}
