#include <hog/flow/region.hpp>
#include <hog/globe/brick.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel.hpp>
#include <hog/globe/voxel_store.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

using hog::flow::Region;
using hog::flow::Stepped;
using hog::globe::BrickSite;
using hog::globe::BrickVoxels;
using hog::globe::Generator;
using hog::globe::Material;
using hog::globe::Site;
using hog::globe::Voxel;
using hog::globe::VoxelStore;
using hog::globe::World;

namespace {

constexpr std::int64_t full = Voxel::full;

const Generator& earth() {
    static const Generator generator{hog::globe::Planet{}};
    return generator;
}

// Columns 24 across and 24 along in the middle of face 0, with a floor: all shaped rock up to
// layer `floor`, then open air to the top of the third brick up, or as `shape(i, j, layer)` says
// where given, over the floor. Cell numbers run from the block's own corner.
class Block {
public:
    static constexpr std::uint32_t side = 3 * hog::globe::brick_side;

    explicit Block(int floor, bool hold_sea = false,
                   const std::function<bool(std::uint32_t, std::uint32_t, int)>& shape = {})
        : store_{earth().layers()}, world_{earth(), store_}, floor_{floor},
          corner_i_{(store_.bricks_across() / 2) * hog::globe::brick_side}, corner_j_{corner_i_} {
        const auto first = (static_cast<std::uint32_t>(floor) - 1) / hog::globe::brick_side;
        for (std::uint32_t bl = first; bl < first + 3; ++bl) {
            for (std::uint32_t bj = 0; bj < 3; ++bj) {
                for (std::uint32_t bi = 0; bi < 3; ++bi) {
                    const BrickSite brick{.face = 0,
                                          .i = (corner_i_ / hog::globe::brick_side) + bi,
                                          .j = (corner_j_ / hog::globe::brick_side) + bj,
                                          .layer = bl};
                    BrickVoxels voxels{};
                    for (int k = 0; k < hog::globe::brick_voxels; ++k) {
                        const Site site = hog::globe::site_in(brick, k);
                        const auto layer = static_cast<int>(site.layer);
                        const bool open =
                            layer >= floor && (!shape || shape(site.i - corner_i_,
                                                               site.j - corner_j_, layer - floor));
                        voxels[static_cast<std::size_t>(k)] =
                            open ? Voxel{}
                                 : Voxel{.distance = -Voxel::far, .solid = Material::Basalt};
                    }
                    store_.put(brick, hog::globe::Brick::pack(voxels));
                    bricks_.push_back(brick);
                }
            }
        }
        region_ = std::make_unique<Region>(world_, hold_sea);
        for (const BrickSite brick : bricks_) {
            region_->load(brick);
        }
    }

    [[nodiscard]] Region& region() { return *region_; }
    [[nodiscard]] const VoxelStore& store() const { return store_; }
    [[nodiscard]] const std::vector<BrickSite>& bricks() const { return bricks_; }

    // `layer` counted up from the floor.
    [[nodiscard]] Site at(std::uint32_t i, std::uint32_t j, int layer) const {
        return {.face = 0,
                .i = corner_i_ + i,
                .j = corner_j_ + j,
                .layer = static_cast<std::uint32_t>(floor_ + layer)};
    }
    [[nodiscard]] std::int64_t amount(std::uint32_t i, std::uint32_t j, int layer) {
        return region_->at(at(i, j, layer)).liquid_amount;
    }
    // All the liquid up a column.
    [[nodiscard]] std::int64_t column(std::uint32_t i, std::uint32_t j) {
        std::int64_t total = 0;
        for (int layer = 0; layer < 16; ++layer) {
            total += amount(i, j, layer);
        }
        return total;
    }
    void pour(std::uint32_t i, std::uint32_t j, int layer, std::int64_t amount) {
        region_->pour(at(i, j, layer), static_cast<std::uint16_t>(amount));
    }

    // Steps while anything changes, up to `most` times; how many it took.
    int settle(int most) {
        for (int step = 1; step <= most; ++step) {
            const std::int64_t before = region_->liquid();
            const Stepped stepped = region_->step();
            if (region_->liquid() != before + stepped.from_sea) {
                FAIL("liquid lost or made: " << before << " -> " << region_->liquid());
            }
            if (stepped.changed == 0) {
                return step;
            }
        }
        return most;
    }

private:
    VoxelStore store_;
    World world_;
    int floor_;
    std::uint32_t corner_i_ = 0;
    std::uint32_t corner_j_ = 0;
    std::vector<BrickSite> bricks_;
    std::unique_ptr<Region> region_;
};

// A brick layer whose voxels all stand `above` the sea.
int floor_above_sea(double above) {
    const hog::globe::Layers& layers = earth().layers();
    const int layer = layers.layer_at(earth().sea_level().value_or(0.0) + above);
    return ((layer / hog::globe::brick_side) + 1) * hog::globe::brick_side;
}

} // namespace

TEST_CASE("liquid poured into the air falls to the floor, and nothing is lost", "[flow][region]") {
    Block block{floor_above_sea(100.0)};
    for (int layer = 5; layer < 8; ++layer) {
        block.pour(12, 12, layer, full);
    }
    const std::int64_t before = block.region().liquid();
    CHECK(before == 3 * full);
    const Stepped stepped = block.region().step();
    CHECK(stepped.changed > 0);
    CHECK(block.region().liquid() == before);
    // Three full on the floor of a column of one; it spread sideways as it fell.
    CHECK(block.amount(12, 12, 0) == full);
    CHECK(block.amount(12, 12, 6) == 0);
    CHECK(block.column(12, 12) < 3 * full);
    CHECK(block.region().flux(0, block.at(12, 12, 0).i, block.at(12, 12, 0).j).along_i > 0);
}

TEST_CASE("a column of liquid spreads the same way along each side and lies flat at last",
          "[flow][region]") {
    Block block{floor_above_sea(100.0)};
    for (int layer = 0; layer < 10; ++layer) {
        block.pour(12, 12, layer, full);
    }
    for (int step = 0; step < 4; ++step) {
        block.region().step();
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
    // Standing against the edge of what's held, it would run on into the bricks beside.
    const Stepped stepped = block.region().step();
    CHECK(!stepped.wanted.empty());
    for (const BrickSite brick : stepped.wanted) {
        CHECK(!block.region().holds(brick));
        CHECK(brick.layer == block.bricks()[9].layer);
    }
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
    const Stepped stepped = block.region().step();
    CHECK(stepped.changed == 0);
    CHECK(stepped.from_sea == 0);
    CHECK(stepped.wanted.empty());
}

TEST_CASE("the world as generated is at rest: a step moves nothing and writes nothing",
          "[flow][region]") {
    const Generator& generator = earth();
    const hog::globe::Layers& layers = generator.layers();
    VoxelStore store{layers};
    World world{generator, store};
    Region region{world};
    for (std::uint64_t k = 1; k <= 4; ++k) {
        const hog::globe::Cell cell = hog::globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304);
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
    const Stepped stepped = region.step();
    CHECK(stepped.changed == 0);
    CHECK(stepped.from_sea == 0);
    CHECK(region.flush().empty());
    CHECK(store.size() == 0);
}
