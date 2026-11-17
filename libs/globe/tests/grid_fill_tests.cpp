#include <hog/core/hash.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/testing/quick.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

using hog::DVec3;
using hog::globe::fill;
using hog::globe::Filled;
using hog::globe::grid_of;
using hog::grid::Key;

namespace {

constexpr std::uint8_t levels = 5;

// A direction whose ground stands over the sea, or under it by `under` metres or more.
DVec3 spread_until(bool land, double under = 0.0) {
    const auto& world = hog::testing::small_world();
    const double sea = world.sea_level().value_or(world.planet().body.radius);
    for (std::uint64_t n = 0;; ++n) {
        const DVec3 up =
            hog::globe::center(hog::globe::cell_at_index(7, (n * 2'654'435'761U) % 98'304U));
        const double ground =
            hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value_or(0.0);
        if (land ? ground > sea + 5.0 : ground < sea - under) {
            return up;
        }
    }
}

// How far a point lies out of the solid as the filled shape has it, in metres: by the plane of
// the leaf over it, or a node's side either way where the leaf is all solid or all open.
double held(const hog::grid::Grid& grid, Key key, const Filled& filled, DVec3 point) {
    const Key foot = grid.key_at(point, static_cast<std::uint8_t>(key.depth + levels)).value();
    const auto down = [&](std::uint32_t at, std::uint32_t from) { return at - (from << levels); };
    const auto leaf =
        filled.shape.at(levels, down(foot.x, key.x), down(foot.y, key.y), down(foot.z, key.z));
    const Key over = foot.ancestor(static_cast<std::uint8_t>(key.depth + leaf.depth));
    const double side = grid.size(over.depth);
    if (!leaf.value.crosses()) {
        return leaf.value.is_open() ? side : -side;
    }
    return (leaf.value.distance() * side) +
           glm::dot(leaf.value.normal(), point - grid.centre(over));
}

} // namespace

TEST_CASE("the grid holds the world down to the layers' thickness", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    CHECK(grid.finest() == world.layers().thickness);
    CHECK(grid.size(0) >= 2.0 * world.layers().ceiling());
}

TEST_CASE("open sky and deep rock are each one leaf", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const auto depth = static_cast<std::uint8_t>(grid.depth() - levels);
    const DVec3 up = spread_until(true);

    const Key sky = grid.key_at(up * (world.layers().ceiling() - 50.0), depth).value();
    const Filled air = fill(world, grid, sky, levels);
    CHECK(air.shape.nodes() == 1);
    CHECK(air.shape.root().value.is_open());
    CHECK_FALSE(air.shape.root().open);
    CHECK(air.material.root().value == hog::globe::palette_of(hog::globe::Material::Air));

    const Key deep = grid.key_at(up * (world.planet().body.radius * 0.5), depth).value();
    const Filled rock = fill(world, grid, deep, levels);
    CHECK(rock.shape.nodes() == 1);
    CHECK(rock.shape.root().value.is_solid());
    CHECK(rock.material.root().value != hog::globe::palette_of(hog::globe::Material::Air));
    CHECK(rock.water.root().value == 0);
}

TEST_CASE("ground splits only near its surface and follows the generator", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const DVec3 up = spread_until(true);
    const double ground = hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value();
    const Key key =
        grid.key_at(up * ground, static_cast<std::uint8_t>(grid.depth() - levels)).value();
    const Filled filled = fill(world, grid, key, levels);

    const std::size_t foot = std::size_t{1} << (3U * levels);
    CHECK(filled.shape.nodes() > 1);
    CHECK(filled.shape.leaves() < foot / 4);

    const double side = grid.size(key.depth);
    const double fine = grid.size(static_cast<std::uint8_t>(key.depth + levels));
    const DVec3 low = grid.centre(key) - (side / 2.0);
    int checked = 0;
    for (std::uint64_t n = 0; n < 400; ++n) {
        const DVec3 point = low + (DVec3{static_cast<double>(hog::hash_values(1, n) % 1000),
                                         static_cast<double>(hog::hash_values(2, n) % 1000),
                                         static_cast<double>(hog::hash_values(3, n) % 1000)} *
                                   (side / 1000.0));
        const double truth = world.sample(point, fine).distance;
        if (std::abs(truth) < 2.0 * fine) {
            continue;
        }
        ++checked;
        CHECK((held(grid, key, filled, point) > 0.0) == (truth > 0.0));
    }
    CHECK(checked > 100);
}

TEST_CASE("solid ground has its material and open space none", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const DVec3 up = spread_until(true);
    const double ground = hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value();
    const Key key =
        grid.key_at(up * ground, static_cast<std::uint8_t>(grid.depth() - levels)).value();
    const Filled filled = fill(world, grid, key, levels);
    const auto air = hog::globe::palette_of(hog::globe::Material::Air);
    const auto foot = static_cast<std::uint8_t>(key.depth + levels);
    const double fine = grid.size(foot);
    const std::uint32_t across = 1U << levels;
    for (std::uint32_t z = 0; z < across; z += 3) {
        for (std::uint32_t y = 0; y < across; y += 3) {
            for (std::uint32_t x = 0; x < across; x += 3) {
                const Key at{.depth = foot,
                             .x = (key.x << levels) + x,
                             .y = (key.y << levels) + y,
                             .z = (key.z << levels) + z};
                const double out = held(grid, key, filled, grid.centre(at));
                const auto material = filled.material.at(levels, x, y, z).value;
                if (out > fine) {
                    CHECK(material == air);
                } else if (out < -fine) {
                    CHECK(material != air);
                }
            }
        }
    }
}

TEST_CASE("open space under the sea is the sea's basin", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const double sea = world.sea_level().value();
    const DVec3 up = spread_until(false, 40.0);
    const auto depth = static_cast<std::uint8_t>(grid.depth() - levels);

    const Key under = grid.key_at(up * (sea - 10.0), depth).value();
    const Filled water = fill(world, grid, under, levels);
    CHECK(water.shape.root().value.is_open());
    CHECK(water.water.nodes() == 1);
    CHECK(water.water.root().value == hog::globe::sea_body);

    const Key sky = grid.key_at(up * (sea + 200.0), depth).value();
    CHECK(fill(world, grid, sky, levels).water.root().value == 0);
}

TEST_CASE("a node filled coarse agrees with the fine ones under it", "[globe][grid]") {
    const auto& world = hog::testing::small_world();
    const hog::grid::Grid grid = grid_of(world);
    const DVec3 up = spread_until(true);
    const double ground = hog::globe::ground_below(world, up, hog::globe::voxel_lod(world)).value();
    // Over every depth, as caves showing only finer turn a few nodes' planes.
    int crossing = 0;
    double apart = 0.0;
    double alike = 0.0;
    const int step = hog::testing::full_or(1, 4);
    for (int at = 4; std::cmp_less(at, grid.depth()); at += step) {
        CAPTURE(at);
        const auto depth = static_cast<std::uint8_t>(at);
        const auto under = static_cast<std::uint8_t>(std::min(3, grid.depth() - depth));
        const Key middle = grid.key_at(up * ground, depth).value();
        // The node on the ground and those beside its faces.
        const std::array<std::array<int, 3>, 7> around{
            {{0, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};
        for (const auto& [dx, dy, dz] : around) {
            const auto key = middle.beside(dx, dy, dz);
            if (!key) {
                continue;
            }
            const Filled coarse = fill(world, grid, *key, 0);
            const Filled fine = fill(world, grid, *key, under);
            const auto was = coarse.shape.root();
            const auto is = fine.shape.root();
            // Nothing finer comes where the coarse said none would, and solid never turns open.
            if (!was.value.crosses() && !was.open) {
                CHECK(fine.shape.nodes() == 1);
                CHECK(is.value == was.value);
            }
            CHECK_FALSE((was.value.is_solid() && is.value.is_open()));
            CHECK_FALSE((was.value.is_open() && is.value.is_solid()));
            if (was.value.crosses() && is.value.crosses()) {
                ++crossing;
                const double off = std::abs(was.value.distance() - is.value.distance());
                CHECK(off < 1.0);
                apart += off;
                alike += glm::dot(was.value.normal(), is.value.normal());
            }
            // The sea's basin is never missed coarse, and solid shows its material at both.
            if (fine.water.root().value != 0) {
                CHECK(coarse.water.root().value == fine.water.root().value);
            }
            const auto air = hog::globe::palette_of(hog::globe::Material::Air);
            CHECK((coarse.material.root().value == air) == was.value.is_open());
            CHECK((fine.material.root().value == air) == is.value.is_open());
        }
    }
    REQUIRE(crossing > 0);
    CHECK(apart / crossing < 0.25);
    CHECK(alike / crossing > 0.8);
}
