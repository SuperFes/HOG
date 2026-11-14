#include "block.hpp"

#include <hog/flow/region.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using hog::flow::Region;
using hog::flow::Stepped;
using hog::flow::testing::Block;
using hog::flow::testing::earth;
using hog::flow::testing::floor_above_sea;
using hog::flow::testing::full;
using hog::flow::testing::tick;
using hog::globe::BrickSite;
using hog::globe::BrickVoxels;
using hog::globe::Generator;
using hog::globe::MadeWorld;
using hog::globe::World;

TEST_CASE("liquid poured into the air falls to the floor, and nothing is lost", "[flow][region]") {
    Block block{floor_above_sea(100.0)};
    for (int layer = 5; layer < 8; ++layer) {
        block.pour(12, 12, layer, full);
    }
    const std::int64_t before = block.region().liquid();
    CHECK(before == 3 * full);
    const Stepped stepped = block.region().step(tick);
    CHECK(stepped.changed > 0);
    CHECK(block.region().liquid() == before);
    // Three full on the floor of a column of one; it spread sideways as it fell.
    CHECK(block.amount(12, 12, 0) == full);
    CHECK(block.amount(12, 12, 6) == 0);
    CHECK(block.column(12, 12) < 3 * full);
    CHECK(block.region().flux(0, block.at(12, 12, 0).i, block.at(12, 12, 0).j).along_i > 0);
}

TEST_CASE("water carries how it rises through the year wherever it runs", "[flow][region]") {
    Block block{floor_above_sea(100.0)};
    for (int layer = 0; layer < 6; ++layer) {
        block.pour(12, 12, layer, full, 23, 140);
    }
    for (int step = 0; step < 6; ++step) {
        block.region().step(tick);
    }
    int wet = 0;
    for (std::uint32_t j = 0; j < Block::side; ++j) {
        for (std::uint32_t i = 0; i < Block::side; ++i) {
            const hog::globe::Voxel voxel = block.voxel(i, j, 0);
            if (voxel.liquid_amount > 0) {
                ++wet;
                CHECK(voxel.rise == 23);
                CHECK(voxel.peak == 140);
            } else {
                CHECK(voxel.rise == 0);
            }
        }
    }
    CHECK(wet > 1);
}

TEST_CASE("a column of liquid spreads the same way along each side and lies flat at last",
          "[flow][region]") {
    Block block{floor_above_sea(100.0)};
    for (int layer = 0; layer < 10; ++layer) {
        block.pour(12, 12, layer, full);
    }
    for (int step = 0; step < 4; ++step) {
        block.region().step(tick);
    }
    for (std::uint32_t k = 1; k <= 4; ++k) {
        const std::int64_t east = block.amount(12 + k, 12, 0);
        CHECK(east > 0);
        CHECK(block.amount(12 - k, 12, 0) == east);
        CHECK(block.amount(12, 12 + k, 0) == east);
        CHECK(block.amount(12, 12 - k, 0) == east);
    }
    REQUIRE(block.settle(4'000) < 4'000);
    CHECK(block.region().liquid() == 10 * full);
    std::int64_t least = full;
    std::int64_t most = 0;
    for (std::uint32_t j = 0; j < Block::side; ++j) {
        for (std::uint32_t i = 0; i < Block::side; ++i) {
            least = std::min(least, block.amount(i, j, 0));
            most = std::max(most, block.amount(i, j, 0));
            CHECK(block.amount(i, j, 1) == 0);
        }
    }
    // Flat to within the step the integer flow can't close, a few units a column.
    CHECK(most - least <= 7 * 2 * (Block::side - 1));
    CHECK(least > 300);
}

TEST_CASE("liquid on the move against the edge of what's held wants the bricks beyond, and still "
          "liquid wants nothing",
          "[flow][region]") {
    Block block{floor_above_sea(100.0)};
    block.pour(0, 0, 3, full);
    const Stepped stepped = block.region().step(tick);
    CHECK(!stepped.wanted.empty());
    for (const BrickSite brick : stepped.wanted) {
        CHECK(!block.region().holds(brick));
        CHECK(brick.layer == block.bricks()[9].layer);
    }
    REQUIRE(block.settle(4'000) < 4'000);
    CHECK(block.region().step(tick).wanted.empty());
}

TEST_CASE("the bricks held are those loaded and not let go", "[flow][region]") {
    Block block{floor_above_sea(100.0)};
    std::vector<BrickSite> held = block.region().held();
    std::ranges::sort(held);
    std::vector<BrickSite> loaded = block.bricks();
    std::ranges::sort(loaded);
    CHECK(held == loaded);
    block.region().unload(loaded.front());
    held = block.region().held();
    CHECK(held.size() == loaded.size() - 1);
    CHECK(std::ranges::find(held, loaded.front()) == held.end());
}

TEST_CASE("liquid finds its level through a tunnel", "[flow][region]") {
    // Two shafts 16 deep, a tunnel of seven columns along the floor between them.
    const auto shape = [](std::uint32_t i, std::uint32_t j, int layer) {
        if (j != 12) {
            return false;
        }
        return i == 4 || i == 12 || (i > 4 && i < 12 && layer == 0);
    };
    Block block{floor_above_sea(100.0), false, shape};
    for (int layer = 0; layer < 16; ++layer) {
        block.pour(4, 12, layer, full);
    }
    REQUIRE(block.settle(20'000) < 20'000);
    CHECK(block.region().liquid() == 16 * full);
    for (std::uint32_t i = 5; i < 12; ++i) {
        CHECK(block.amount(i, 12, 0) >= full);
        CHECK(block.amount(i, 12, 1) == 0);
    }
    // The shafts' heads within a hair, the tunnel pressed full between them.
    const std::int64_t here = block.column(4, 12);
    const std::int64_t there = block.column(12, 12);
    CHECK(std::abs(here - there) <= 7 * 7);
    CHECK(here > 4 * full);
    CHECK(block.amount(4, 12, 4) > 0);
    CHECK(block.amount(4, 12, 5) == 0);
}

TEST_CASE("a tank drains through a hole in its side down to the hole, and no further",
          "[flow][region]") {
    // Two shafts with a wall between, holed ten up.
    const auto shape = [](std::uint32_t i, std::uint32_t j, int layer) {
        if (j != 12) {
            return false;
        }
        return i == 4 || i == 6 || (i == 5 && layer == 10);
    };
    Block block{floor_above_sea(100.0), false, shape};
    for (int layer = 0; layer < 16; ++layer) {
        block.pour(4, 12, layer, full);
    }
    REQUIRE(block.settle(20'000) < 20'000);
    CHECK(block.region().liquid() == 16 * full);
    CHECK(block.column(4, 12) >= 10 * full);
    CHECK(block.column(4, 12) <= (10 * full) + 7);
    CHECK(block.column(6, 12) >= (6 * full) - 7);
    CHECK(block.amount(6, 12, 5) >= full - 7);
    CHECK(block.amount(6, 12, 6) == 0);
}

TEST_CASE("the sea beyond the edge fills a hollow to its level and no higher", "[flow][region]") {
    const hog::globe::Layers& layers = earth().layers();
    const double sea = earth().sea_level().value_or(0.0);
    // A floor six layers under the sea's level, the open air over it reaching above.
    const int floor = layers.layer_at(sea) - 6;
    Block block{floor, true};
    REQUIRE(block.region().liquid() == 0);
    REQUIRE(block.settle(4'000) < 4'000);
    for (std::uint32_t j = 0; j < Block::side; ++j) {
        for (std::uint32_t i = 0; i < Block::side; ++i) {
            for (int layer = 0; layer < 16; ++layer) {
                const bool under = layers.middle(floor + layer) < sea;
                // Full to within the few units a column the integer flow leaves.
                CHECK(block.amount(i, j, layer) >= (under ? full - (std::int64_t{7} * 24) : 0));
                CHECK(block.amount(i, j, layer) <= (under ? full : 0));
            }
        }
    }
    const Stepped stepped = block.region().step(tick);
    CHECK(stepped.changed == 0);
    CHECK(stepped.from_sea == 0);
    CHECK(stepped.wanted.empty());
}

TEST_CASE("the world as generated is at rest: a step moves nothing and writes nothing",
          "[flow][region]") {
    const Generator& generator = earth();
    const hog::globe::Layers& layers = generator.layers();
    MadeWorld made{generator};
    World& world = made.world();
    Region region{world};
    for (std::uint64_t k = 1; k <= 4; ++k) {
        const hog::globe::Cell cell = hog::globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304);
        made.cover(hog::globe::center(cell));
        const double ground =
            hog::globe::ground_below(generator, hog::globe::center(cell), 0.5).value_or(0.0);
        const hog::globe::Cell column =
            hog::globe::cell_under(hog::globe::center(cell), layers.level);
        const BrickSite at =
            hog::globe::brick_of({.face = column.face,
                                  .i = column.i,
                                  .j = column.j,
                                  .layer = static_cast<std::uint32_t>(layers.layer_at(ground))});
        for (std::uint32_t dl = 0; dl < 3; ++dl) {
            for (std::uint32_t dj = 0; dj < 3; ++dj) {
                for (std::uint32_t di = 0; di < 3; ++di) {
                    region.load({.face = at.face,
                                 .i = at.i + di - 1,
                                 .j = at.j + dj - 1,
                                 .layer = at.layer + dl - 1});
                }
            }
        }
    }
    const Stepped stepped = region.step(tick);
    CHECK(stepped.changed == 0);
    CHECK(stepped.from_sea == 0);
    CHECK(region.flush().empty());
    CHECK(world.revision() == 0);
}

TEST_CASE("a puddle soaks into the ground as fast as the ground lets it: sand within the hour, "
          "soil in a day, mud hardly at all",
          "[flow][region]") {
    // One column open, walled all round.
    const auto shaft = [](std::uint32_t i, std::uint32_t j, int /*layer*/) {
        return i == 12 && j == 12;
    };
    constexpr double hours = 6.0 * 3'600.0;
    const auto puddle_left = [&](hog::globe::Material ground) {
        Block block{floor_above_sea(100.0), false, shaft, ground};
        block.pour(12, 12, 0, full / 2);
        for (int step = 0; step < 6; ++step) {
            const std::int64_t before = block.region().liquid();
            block.region().step(hours / 6.0);
            REQUIRE(block.region().liquid() == before);
        }
        // What's gone from the puddle is in the ground under it.
        const std::int64_t left = block.amount(12, 12, 0);
        std::int64_t in_ground = 0;
        for (int layer = -1; layer >= -8; --layer) {
            in_ground += block.amount(12, 12, layer);
        }
        CHECK(in_ground == (full / 2) - left);
        const double seeped = hog::globe::properties(ground).conductivity * hours /
                              earth().layers().thickness * static_cast<double>(full);
        const auto expected = static_cast<std::int64_t>(std::max(0.0, (full / 2.0) - seeped));
        CHECK(std::abs(left - expected) <= 64);
        return left;
    };
    CHECK(puddle_left(hog::globe::Material::Sand) == 0);
    CHECK(puddle_left(hog::globe::Material::Soil) > 0);
    CHECK(puddle_left(hog::globe::Material::Sediment) > (full / 2) - 400);
}

TEST_CASE("water in the ground drains down to what the ground keeps, and no further",
          "[flow][region]") {
    const auto shaft = [](std::uint32_t i, std::uint32_t j, int /*layer*/) {
        return i == 12 && j == 12;
    };
    Block block{floor_above_sea(100.0), false, shaft, hog::globe::Material::Sand};
    const std::int64_t pores =
        hog::globe::amount_of(hog::globe::properties(hog::globe::Material::Sand).porosity);
    block.pour(12, 12, -1, pores);
    for (int step = 0; step < 48; ++step) {
        block.region().step(3'600.0);
    }
    CHECK(block.amount(12, 12, -1) == pores / 2);
    CHECK(block.amount(12, 12, -2) == pores / 2);
    CHECK(block.amount(12, 12, -1) + block.amount(12, 12, -2) + block.amount(12, 12, -3) == pores);
    CHECK(block.amount(12, 12, 0) == 0);
}

TEST_CASE("a current scours a soft bed and carries it off; what it carries settles out once the "
          "water stands",
          "[flow][region]") {
    using hog::globe::Material;
    const auto trench = [](std::uint32_t /*i*/, std::uint32_t j, int /*layer*/) { return j == 12; };
    Block block{floor_above_sea(100.0), false, trench, Material::Soil};
    const std::int64_t bed_before = block.solid(1, 12, -1);
    for (int layer = 0; layer < 8; ++layer) {
        block.pour(1, 12, layer, full);
    }
    block.settle(200);
    // The head running off the source scoured the bed there, and the soil is on the move.
    CHECK(block.solid(1, 12, -1) < bed_before);
    CHECK(block.region().grain() > 0);
    // Once the water lies level, what it carried lies down again as soil on the trench's floor.
    block.settle(3'000);
    block.settle(60, 60.0);
    std::int64_t carried = 0;
    std::int64_t piled = 0;
    for (std::uint32_t i = 0; i < Block::side; ++i) {
        carried += block.carried(i, 12);
        piled += block.grain(i, 12, 0);
    }
    CHECK(carried == 0);
    CHECK(piled == block.region().grain());
    CHECK(piled > 0);
}

TEST_CASE("still water wears nothing off its bed", "[flow][region]") {
    using hog::globe::Material;
    const auto shaft = [](std::uint32_t i, std::uint32_t j, int /*layer*/) {
        return i == 12 && j == 12;
    };
    Block block{floor_above_sea(100.0), false, shaft, Material::Sand};
    const std::int64_t bed_before = block.solid(12, 12, -1);
    for (int layer = 0; layer < 8; ++layer) {
        block.pour(12, 12, layer, full);
    }
    block.settle(200);
    CHECK(block.solid(12, 12, -1) == bed_before);
    CHECK(block.region().grain() == 0);
}

TEST_CASE("carried sand settles out at once, carried mud over hours", "[flow][region]") {
    using hog::globe::Material;
    const auto shaft = [](std::uint32_t i, std::uint32_t j, int /*layer*/) {
        return i == 12 && j == 12;
    };
    Block sandy{floor_above_sea(100.0), false, shaft};
    sandy.pour(12, 12, 0, full);
    sandy.suspend(12, 12, 0, Material::Sand, 1000);
    sandy.settle(1, 60.0);
    CHECK(sandy.carried(12, 12) == 0);
    CHECK(sandy.grain(12, 12, 0) == 1000);
    CHECK(sandy.region().at(sandy.at(12, 12, 0)).grain == Material::Sand);

    Block muddy{floor_above_sea(100.0), false, shaft};
    muddy.pour(12, 12, 0, full);
    muddy.suspend(12, 12, 0, Material::Sediment, 1000);
    muddy.settle(1, 60.0);
    CHECK(muddy.carried(12, 12) > 900);
    for (int hour = 0; hour < 24; ++hour) {
        muddy.settle(1, 3'600.0);
    }
    CHECK(muddy.carried(12, 12) == 0);
    CHECK(muddy.grain(12, 12, 0) == 1000);
}

TEST_CASE("grain piled on the bed takes room from the liquid and stands under its level",
          "[flow][region]") {
    using hog::globe::Material;
    const auto pair = [](std::uint32_t i, std::uint32_t j, int /*layer*/) {
        return (i == 11 || i == 12) && j == 12;
    };
    Block block{floor_above_sea(100.0), false, pair};
    block.pile(11, 12, 0, Material::Basalt, full / 2);
    block.pour(12, 12, 1, full);
    block.settle(200);
    CHECK(block.amount(11, 12, 0) <= full / 2);
    CHECK(block.grain(11, 12, 0) == full / 2);
    const std::int64_t over_pile = (full / 2) + block.column(11, 12);
    CHECK(std::abs(over_pile - block.column(12, 12)) <= 7);
}

TEST_CASE("a tall heap of sand on level ground slumps wider, losing none, and then sleeps",
          "[flow][region][slump]") {
    using hog::globe::Material;
    Block block{floor_above_sea(100.0)};
    for (int layer = 0; layer < 12; ++layer) {
        block.pile(12, 12, layer, Material::Sand, full);
    }
    const std::int64_t before = block.region().grain();

    std::int64_t last = 1;
    for (int step = 0; step < 4000 && last > 0; ++step) {
        const hog::flow::Stepped stepped = block.region().step(tick);
        // Grain still in the air isn't at rest.
        last = stepped.slumped + static_cast<std::int64_t>(stepped.flying);
    }

    CHECK(last == 0);
    CHECK(block.region().grain() == before);
    CHECK(block.grain(12, 12, 11) == 0);
    CHECK(block.grain(13, 12, 0) > 0);
    CHECK(block.grain(13, 13, 0) > 0);
    // Spread the same way along an axis and across a diagonal, to a voxel or two.
    const auto reach = [&](int di, int dj) {
        int steps = 0;
        while (block.grain(static_cast<std::uint32_t>(12 + ((steps + 1) * di)),
                           static_cast<std::uint32_t>(12 + ((steps + 1) * dj)), 0) > 0) {
            ++steps;
        }
        return steps * std::hypot(di, dj);
    };
    CHECK(reach(1, 1) == Catch::Approx(reach(1, 0)).margin(2.0));
    CHECK(reach(1, 0) >= 2.0);
}

TEST_CASE("sand piled on a cave's floor under a roof slumps there, losing none, and sleeps",
          "[flow][region][slump]") {
    using hog::globe::Material;
    // A cave six voxels high under solid rock.
    const auto shape = [](std::uint32_t, std::uint32_t, int layer) { return layer < 6; };
    Block block{floor_above_sea(100.0), false, shape};
    for (int layer = 0; layer < 5; ++layer) {
        block.pile(12, 12, layer, Material::Sand, full);
    }
    const std::int64_t before = block.region().grain();

    bool resting = false;
    for (int step = 0; step < 4000 && !resting; ++step) {
        const hog::flow::Stepped stepped = block.region().step(tick);
        resting = stepped.slumped == 0 && stepped.flying == 0;
    }

    CHECK(resting);
    CHECK(block.region().grain() == before);
    CHECK(block.grain(12, 12, 4) == 0);
    CHECK(block.grain(13, 12, 0) > 0);
    CHECK(block.grain(12, 13, 0) > 0);
}

TEST_CASE("grain with nothing under it falls, over seconds, and lands on the floor",
          "[flow][region][slump]") {
    using hog::globe::Material;
    Block block{floor_above_sea(100.0)};
    // One full voxel of sand hanging eight voxels up in the open.
    block.pile(12, 12, 8, Material::Sand, full);
    const std::int64_t before = block.region().grain();

    double airborne = 0.0;
    bool resting = false;
    for (int step = 0; step < 4000 && !resting; ++step) {
        const hog::flow::Stepped stepped = block.region().step(tick);
        if (stepped.flying > 0) {
            airborne += tick;
            CHECK(block.grain(12, 12, 0) == 0);
        }
        resting = stepped.slumped == 0 && stepped.flying == 0;
    }

    CHECK(resting);
    CHECK(airborne > 0.5);
    CHECK(block.region().grain() == before);
    CHECK(block.grain(12, 12, 8) == 0);
    CHECK(block.grain(12, 12, 0) > 0);
}

TEST_CASE("sand at the lip of a drop deeper than eight voxels runs down it, losing none",
          "[flow][region][slump]") {
    using hog::globe::Material;
    // Rock 14 high up to column 11, open ground below it beyond; the sand on it stands a voxel
    // under the block's ceiling.
    const auto shape = [](std::uint32_t i, std::uint32_t, int layer) {
        return i >= 12 || layer >= 14;
    };
    Block block{floor_above_sea(100.0), false, shape};
    block.pile(11, 12, 14, Material::Sand, full);
    const std::int64_t before = block.region().grain();

    const auto below = [&] {
        std::int64_t sum = 0;
        for (std::uint32_t i = 12; i < Block::side; ++i) {
            for (std::uint32_t j = 0; j < Block::side; ++j) {
                sum += block.grain(i, j, 0);
            }
        }
        return sum;
    };

    // It goes over the lip as grain in flight, which takes time to arrive: seconds, not a step.
    double airborne = 0.0;
    bool resting = false;
    for (int step = 0; step < 4000 && !resting; ++step) {
        const hog::flow::Stepped stepped = block.region().step(tick);
        if (stepped.flying > 0) {
            airborne += tick;
            CHECK(block.region().grain() == before);
        }
        resting = stepped.slumped == 0 && stepped.flying == 0;
        if (airborne > 0.0 && airborne < 1.0) {
            CHECK(below() == 0);
        }
    }

    CHECK(resting);
    CHECK(airborne > 1.0);
    CHECK(block.region().grain() == before);
    CHECK(below() > 0);
}

TEST_CASE("a dusting of snow clings to a steep slope that a deep drift slides down",
          "[flow][region][slump]") {
    using hog::globe::Material;
    // Rock rising a voxel for each column along i: a slope of one in one.
    const auto ramp = [](std::uint32_t i, std::uint32_t, int layer) {
        return std::cmp_greater_equal(layer, std::min<std::uint32_t>(i, 16));
    };
    const auto lay = [&](Block& block, std::int64_t amount, int depth) {
        for (std::uint32_t i = 4; i < 13; ++i) {
            for (std::uint32_t j = 8; j < 17; ++j) {
                for (int up = 0; up < depth; ++up) {
                    block.pile(i, j, static_cast<int>(i) + up, Material::Snow, amount);
                }
            }
        }
    };

    Block thin{floor_above_sea(100.0), false, ramp};
    lay(thin, full / 8, 1);
    const std::int64_t dusting = thin.region().grain();
    REQUIRE(dusting > 0);
    std::int64_t thin_moved = 0;
    for (int step = 0; step < 400; ++step) {
        const hog::flow::Stepped stepped = thin.region().step(tick);
        thin_moved += stepped.slumped + static_cast<std::int64_t>(stepped.flying);
    }
    CHECK(thin_moved == 0);
    CHECK(thin.region().grain() == dusting);

    Block deep{floor_above_sea(100.0), false, ramp};
    lay(deep, full, 4);
    const std::int64_t drift = deep.region().grain();
    std::int64_t deep_moved = 0;
    for (int step = 0; step < 400; ++step) {
        const hog::flow::Stepped stepped = deep.region().step(tick);
        deep_moved += stepped.slumped;
    }
    CHECK(deep_moved > 0);
    // It ran down onto rock below the drift's foot.
    CHECK(deep.grain(2, 12, 2) > 0);
    CHECK(deep.region().grain() == drift);
}

TEST_CASE("a lump thrown onto a heap's flank rolls off down it instead of dropping in",
          "[flow][region][slump]") {
    using hog::globe::Material;
    Block block{floor_above_sea(100.0)};
    // Rubble stepping down a voxel every second column along i: well under what it holds.
    for (std::uint32_t i = 0; i < Block::side; ++i) {
        for (std::uint32_t j = 0; j < Block::side; ++j) {
            for (int up = 0; up < std::max(0, (17 - static_cast<int>(i)) / 2); ++up) {
                block.pile(i, j, up, Material::Granite, full);
            }
        }
    }
    // Let the heap sleep first, so the only grain in motion is the lump.
    bool resting = false;
    for (int step = 0; step < 4000 && !resting; ++step) {
        const hog::flow::Stepped stepped = block.region().step(tick);
        resting = stepped.slumped == 0 && stepped.flying == 0;
    }
    REQUIRE(resting);
    const std::int64_t before = block.region().grain();
    const auto& layers = block.world().layers();
    block.region().throw_grain(hog::globe::middle_of(block.at(12, 12, 5), layers), {},
                               Material::Granite, full);

    std::optional<hog::DVec3> start;
    hog::DVec3 last{};
    bool landed = false;
    for (int step = 0; step < 2000 && !landed; ++step) {
        for (const auto& parcel : block.region().parcels()) {
            last = parcel.at;
            start = start.value_or(parcel.at);
        }
        const hog::flow::Stepped stepped = block.region().step(tick);
        landed = stepped.flying == 0;
    }

    REQUIRE(landed);
    REQUIRE(start);
    CHECK(block.region().grain() == before + full);
    // It went on along the flank, not straight down where it hit.
    const double along =
        std::acos(std::min(1.0, glm::dot(glm::normalize(start.value()), glm::normalize(last)))) *
        glm::length(last);
    CHECK(along > 0.1);
}

TEST_CASE("sand heaped at a cube edge slumps over it onto the next face, losing none",
          "[flow][region][slump]") {
    using hog::globe::Material;
    const int level = hog::flow::testing::earth().layers().level;
    const std::uint32_t last = (std::uint32_t{1} << level) - 1;
    const std::uint32_t j = last / 2;
    Block block{floor_above_sea(100.0), Block::Edge{}, j};
    const auto layer = static_cast<std::uint32_t>(block.floor());
    for (std::uint32_t up = 0; up < 12; ++up) {
        block.region().pile({.face = 0, .i = last, .j = j, .layer = layer + up}, Material::Sand,
                            static_cast<std::uint16_t>(full));
    }
    const std::int64_t before = block.region().grain();

    bool resting = false;
    for (int step = 0; step < 4000 && !resting; ++step) {
        const hog::flow::Stepped stepped = block.region().step(tick);
        resting = stepped.slumped == 0 && stepped.flying == 0;
    }

    REQUIRE(resting);
    CHECK(block.region().grain() == before);
    const auto over = hog::globe::next_to(
        {.level = static_cast<std::uint8_t>(level), .face = 0, .i = last, .j = j}, 1, 0);
    REQUIRE(over);
    const hog::globe::Cell beyond = over.value();
    CHECK(beyond.face != 0);
    CHECK(block.region()
              .at({.face = beyond.face, .i = beyond.i, .j = beyond.j, .layer = layer})
              .grain_amount > 0);
    CHECK(block.region().at({.face = 0, .i = last - 1, .j = j, .layer = layer}).grain_amount > 0);
}

TEST_CASE("a tall pile of ice stays put until the years run, then spreads without losing any",
          "[flow][region][glacier]") {
    using hog::globe::Material;
    Block block{floor_above_sea(100.0)};
    for (int layer = 0; layer < 12; ++layer) {
        block.pile(12, 12, layer, Material::Ice, full);
    }
    const std::int64_t before = block.region().grain();

    // Ice isn't a heap of loose grain: with no years given it holds its shape.
    for (int step = 0; step < 20; ++step) {
        block.region().step(tick);
    }
    CHECK(block.grain(12, 12, 11) == full);
    CHECK(block.grain(13, 12, 0) == 0);

    block.region().set_ice_years_per_second(0.5);
    std::int64_t moved = 0;
    for (int step = 0; step < 400; ++step) {
        moved += block.region().step(tick).glaciated;
    }
    CHECK(moved > 0);
    CHECK(block.grain(13, 12, 0) > 0);
    CHECK(block.grain(12, 12, 11) < full);
    CHECK(block.region().grain() == before);
}

TEST_CASE("snow above the snowline grows ice on bare ground, and melt takes it away",
          "[flow][region][glacier]") {
    using hog::globe::Material;
    Block block{floor_above_sea(100.0)};
    double rate = 20.0;
    block.region().set_ice_years_per_second(1.0);
    block.region().set_ice_balance([&](const hog::globe::Cell&, double) { return rate; });
    for (int step = 0; step < 20; ++step) {
        block.region().step(tick);
    }
    const std::int64_t grown = block.region().grain();
    CHECK(grown > 0);
    CHECK(block.grain(12, 12, 0) > 0);

    rate = -20.0;
    for (int step = 0; step < 200; ++step) {
        block.region().step(tick);
    }
    CHECK(block.region().grain() < grown / 10);
}

TEST_CASE("ice aged in one go grows where it is cold and stops where it is warm",
          "[flow][region][glacier]") {
    using hog::globe::Material;
    Block block{floor_above_sea(100.0)};
    const std::uint32_t split = block.at(12, 12, 0).i;
    block.region().set_ice_balance(
        [&](const hog::globe::Cell& column, double) { return column.i < split ? 0.5 : -5.0; });
    std::int64_t moved = 0;
    for (int year = 0; year < 400; ++year) {
        moved += block.region().age_ice(1.0).glaciated;
    }
    CHECK(moved > 0);
    CHECK(block.grain(3, 12, 0) > 0);
    // Ice that flows across the line melts there, so the warm far side stays bare.
    CHECK(block.grain(22, 12, 0) == 0);
}

TEST_CASE("cover aged in one go creeps off a heap and weathers anew on bare ground",
          "[flow][region][creep]") {
    using hog::globe::Material;
    Block block{floor_above_sea(100.0)};
    for (int layer = 0; layer < 3; ++layer) {
        block.pile(12, 12, layer, Material::Soil, full);
    }
    const std::int64_t before = block.grain(12, 12, 2);
    CHECK(before == full);
    std::int64_t moved = 0;
    for (int step = 0; step < 20; ++step) {
        moved += block.region().age_cover(100.0).crept;
    }
    CHECK(moved > 0);
    CHECK(block.grain(12, 12, 2) < before);
    CHECK(block.grain(13, 12, 0) > 0);
    // Bare ground far from the heap has had cover weathered onto it.
    CHECK(block.grain(3, 3, 0) > 0);
}
