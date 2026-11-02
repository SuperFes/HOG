#include <hog/globe/grip.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <numbers>

using namespace hog;
using namespace hog::globe;

namespace {

const Generator& earth() {
    static const Generator generator{Planet{}};
    return generator;
}

constexpr double degrees = std::numbers::pi / 180.0;

// A column of granite up to a brick's boundary, and over it `grain` this full, as wet as `water`.
struct Ground {
    Ground() : store(earth().layers()), world(earth(), store) {
        const Layers& layers = earth().layers();
        const Cell cell = cell_under(DVec3{1.0, 0.0, 0.0}, layers.level);
        top = static_cast<std::uint32_t>(layers.count / 2 / brick_side) * brick_side;
        low = {.face = cell.face,
               .i = cell.i / brick_side,
               .j = cell.j / brick_side,
               .layer = (top / brick_side) - 1};
        high = {.face = low.face, .i = low.i, .j = low.j, .layer = top / brick_side};
        column = cell;
        BrickVoxels rock{};
        for (Voxel& voxel : rock) {
            voxel.solid = Material::Granite;
        }
        world.write(low, rock);
        world.write(high, BrickVoxels{});
    }

    void lay(Material material, double share, double water) {
        BrickVoxels voxels{};
        Voxel& here = voxels[static_cast<std::size_t>(
            ((column.j % brick_side) * brick_side) + (column.i % brick_side))];
        here.grain = material;
        here.grain_amount = amount_of(share);
        if (water > 0.0) {
            here.liquid = Material::Water;
            here.liquid_amount = amount_of(water);
        }
        world.write(high, voxels);
    }

    [[nodiscard]] DVec3 feet() const {
        const Layers& layers = earth().layers();
        return center(column) * (layers.bottom(static_cast<int>(top)) + 0.1);
    }

    VoxelStore store;
    World world;
    Cell column;
    BrickSite low;
    BrickSite high;
    std::uint32_t top = 0;
};

} // namespace

TEST_CASE("footing on bare granite is the rock's grip, less when wet", "[globe][grip]") {
    Ground ground;
    const std::optional<double> dry = grip_at(ground.world, ground.feet());
    REQUIRE(dry);
    CHECK(*dry == Catch::Approx(45.0 * degrees));
    ground.lay(Material::Air, 0.0, 1.0);
    const std::optional<double> wet = grip_at(ground.world, ground.feet());
    REQUIRE(wet);
    CHECK(*wet == Catch::Approx(38.0 * degrees));
}

TEST_CASE("a dusting of snow on granite holds a steep slope, a deep drift much less",
          "[globe][grip]") {
    Ground ground;
    ground.lay(Material::Snow, 0.01, 0.0);
    const std::optional<double> dusting = grip_at(ground.world, ground.feet());
    ground.lay(Material::Snow, 1.0, 0.0);
    const std::optional<double> drift = grip_at(ground.world, ground.feet());
    REQUIRE(dusting);
    REQUIRE(drift);
    CHECK(*dusting > *drift);
    CHECK(*drift < 45.0 * degrees);
}

TEST_CASE("no ground just under the feet gives no grip", "[globe][grip]") {
    Ground ground;
    CHECK_FALSE(grip_at(ground.world, ground.feet() * 1.001));
}
