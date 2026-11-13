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

// Voxels thick enough that a full one is a deep drift.
const Generator& earth() {
    static const Generator generator{[] {
        Planet planet;
        planet.voxel = 2.5;
        return planet;
    }()};
    return generator;
}

constexpr double degrees = std::numbers::pi / 180.0;

// A column of granite up to a brick's boundary, and over it `grain` this full, as wet as `water`.
struct Ground {
    Ground() : world(earth(), volume) {
        const Layers& layers = earth().layers();
        const Cell cell = cell_under(DVec3{1.0, 0.0, 0.0}, layers.level);
        volume.set_floor({.face = cell.face, .i = cell.i, .j = cell.j}, 0);
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
            voxel.solid_amount = Voxel::full;
        }
        world.write(low, rock);
        world.write(high, BrickVoxels{});
    }

    void lay(Material material, double share, double water) {
        BrickVoxels voxels{};
        constexpr auto side = static_cast<std::uint32_t>(brick_side);
        Voxel& here = voxels[(std::size_t{column.j % side} * side) + (column.i % side)];
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

    Volume volume;
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
    CHECK(dry.value() == Catch::Approx(45.0 * degrees));
    ground.lay(Material::Air, 0.0, 1.0);
    const std::optional<double> wet = grip_at(ground.world, ground.feet());
    REQUIRE(wet);
    CHECK(wet.value() == Catch::Approx(38.0 * degrees));
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
    CHECK(dusting.value() > drift.value());
    CHECK(drift.value() < 45.0 * degrees);
}

TEST_CASE("no ground just under the feet gives no grip", "[globe][grip]") {
    const Ground ground;
    CHECK_FALSE(grip_at(ground.world, ground.feet() * 1.001));
}
