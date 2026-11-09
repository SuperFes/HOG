#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/pockets.hpp>
#include <hog/globe/volume_build.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::globe::Generator;
using hog::globe::Pocket;

namespace {

constexpr std::array voxels{1.0, 2.5};

// A small planet, made in seconds, at a voxel size.
const Generator& planet_of(double voxel) {
    static std::vector<std::pair<double, Generator>> made;
    for (const auto& [size, generator] : made) {
        if (size == voxel) {
            return generator;
        }
    }
    hog::globe::Planet planet;
    planet.radius = 4'000.0;
    planet.floor_depth = 100.0;
    planet.sky_height = 500.0;
    planet.voxel = voxel;
    made.emplace_back(voxel, Generator{planet});
    return made.back().second;
}

} // namespace

TEST_CASE("the pockets found under the ground are the hollows in it, near to far",
          "[globe][pockets]") {
    const double voxel = GENERATE(from_range(voxels));
    CAPTURE(voxel);
    const Generator& generator = planet_of(voxel);
    hog::globe::MadeWorld made{generator};
    hog::globe::World& world = made.world();
    const hog::globe::Layers& layers = generator.layers();
    constexpr double range = 80.0;
    const DVec3 up = hog::globe::center(hog::globe::cell_at_index(7, 4'321));
    const DVec3 pole = std::abs(up.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(pole, up));
    const double radius = generator.planet().radius;
    // Tile columns enough to hold the range about the eye, made down past its reach.
    const auto across = static_cast<std::uint32_t>(
        std::ceil((2.0 * range) / (hog::globe::Volume::tile_side * voxel)) + 1.0);
    made.cover(up, across);
    const double ground = hog::globe::top_of(world, up).value_or(0.0);
    made.cover_down(up, layers.layer_at(ground - range), across);
    const DVec3 eye = up * ground;
    // Hollows under the ground, further out each.
    std::vector<DVec3> hollows;
    for (const double out : {0.0, 25.0, 50.0}) {
        const DVec3 there = glm::normalize(up + (east * (out / radius)));
        const DVec3 hollow = there * (hog::globe::top_of(world, there).value_or(0.0) - 20.0);
        hog::globe::dig(world, hollow, 6.0);
        hollows.push_back(hollow);
    }
    const auto started = std::chrono::steady_clock::now();
    const std::vector<Pocket> pockets = hog::globe::find_pockets(world, eye, range, 6.0);
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(took < 5.0);
    for (const DVec3 hollow : hollows) {
        CHECK(std::ranges::any_of(
            pockets, [&](const Pocket& pocket) { return glm::length(pocket.at - hollow) < 6.0; }));
    }
    double last = 0.0;
    for (const Pocket& pocket : pockets) {
        const double away = glm::length(pocket.at - eye);
        CHECK(away >= last);
        CHECK(away < range + pocket.across);
        CHECK(pocket.across > 0.0);
        CHECK(pocket.height > 0.0);
        last = away;
        // A dug hollow's open voxels lie up or down the column it's at, within its own height; a
        // cave's middle may lie off its winding open space.
        const bool dug = std::ranges::any_of(
            hollows, [&](const DVec3 hollow) { return glm::length(pocket.at - hollow) < 6.0; });
        if (!dug) {
            continue;
        }
        const hog::globe::Cell cell =
            hog::globe::cell_under(glm::normalize(pocket.at), layers.level);
        const double middle = glm::length(pocket.at);
        bool open = false;
        for (int layer = layers.layer_at(middle - pocket.height);
             layer <= layers.layer_at(middle + pocket.height) && !open; ++layer) {
            open = world
                       .at({.face = cell.face,
                            .i = cell.i,
                            .j = cell.j,
                            .layer = static_cast<std::uint32_t>(layer)})
                       .solid == hog::globe::Material::Air;
        }
        CHECK(open);
    }
}

TEST_CASE("a hole dug deep in the rock is a pocket", "[globe][pockets]") {
    const Generator& generator = planet_of(2.5);
    hog::globe::MadeWorld made{generator};
    hog::globe::World& world = made.world();
    const DVec3 up = hog::globe::center(hog::globe::cell_at_index(7, 4'321));
    made.cover(up);
    const double ground = hog::globe::top_of(world, up).value_or(0.0);
    // Within what's made under the ground.
    const DVec3 hole = up * (ground - 20.0);
    const auto near_hole = [&] {
        return std::ranges::any_of(
            hog::globe::find_pockets(world, up * ground, 100.0, 6.0),
            [&](const Pocket& pocket) { return glm::length(pocket.at - hole) < 6.0; });
    };
    // Solid rock there before it's dug.
    REQUIRE_FALSE(near_hole());
    hog::globe::dig(world, hole, 6.0);
    CHECK(near_hole());
}
