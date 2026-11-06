#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/pockets.hpp>
#include <hog/globe/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <print>
#include <tuple>
#include <vector>

using hog::DVec3;
using hog::globe::Generator;
using hog::globe::Pocket;

namespace {

constexpr std::array voxels{1.0, 2.5};

// A small worn planet, made in seconds, at a voxel size.
const Generator& planet_of(double voxel, double wear = 0.03) {
    static std::vector<std::tuple<double, double, Generator>> made;
    for (const auto& [size, worn, generator] : made) {
        if (size == voxel && worn == wear) {
            return generator;
        }
    }
    auto planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
    planet.radius = 4'000.0;
    planet.floor_depth = 100.0;
    planet.sky_height = 500.0;
    planet.wear = wear;
    planet.voxel = voxel;
    made.emplace_back(voxel, wear, Generator{planet});
    return std::get<2>(made.back());
}

} // namespace

TEST_CASE("the pockets found under the ground are open air, near to far", "[globe][pockets]") {
    const double voxel = GENERATE(from_range(voxels));
    CAPTURE(voxel);
    const Generator& generator = planet_of(voxel);
    hog::globe::VoxelStore store{generator.layers()};
    const hog::globe::World world{generator, store};
    const hog::globe::Layers& layers = generator.layers();
    constexpr double range = 300.0;
    std::size_t found = 0;
    std::size_t open = 0;
    for (std::uint64_t k = 0; k < 40; ++k) {
        const DVec3 eye =
            hog::globe::center(hog::globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304)) *
            generator.planet().radius;
        const auto started = std::chrono::steady_clock::now();
        const std::vector<Pocket> pockets = hog::globe::find_pockets(world, eye, range, 6.0);
        const double took =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        CHECK(took < 5.0);
        double last = 0.0;
        for (const Pocket& pocket : pockets) {
            const double away = glm::length(pocket.at - eye);
            CHECK(away >= last);
            CHECK(away < range + pocket.across);
            CHECK(pocket.across > 0.0);
            CHECK(pocket.height > 0.0);
            last = away;
            ++found;
            // Open voxels lie up or down the column the pocket is at, within its own height.
            const hog::globe::Cell cell =
                hog::globe::cell_under(glm::normalize(pocket.at), layers.level);
            const double middle = glm::length(pocket.at);
            for (int layer = layers.layer_at(middle - pocket.height);
                 layer <= layers.layer_at(middle + pocket.height); ++layer) {
                const hog::globe::Voxel here =
                    world.at({.face = cell.face,
                              .i = cell.i,
                              .j = cell.j,
                              .layer = static_cast<std::uint32_t>(layer)});
                if (here.solid == hog::globe::Material::Air) {
                    ++open;
                    break;
                }
            }
        }
    }
    std::println("pockets found {} of which open {}", found, open);
    CHECK(found > 0);
    CHECK(open * 10 >= found * 8);
}

TEST_CASE("a hole dug deep in the rock is a pocket", "[globe][pockets]") {
    const Generator& generator = planet_of(2.5);
    hog::globe::VoxelStore store{generator.layers()};
    hog::globe::World world{generator, store};
    const DVec3 up = hog::globe::center(hog::globe::cell_at_index(7, 4'321));
    const double ground = hog::globe::top_of(world, up).value_or(0.0);
    const DVec3 hole = up * (ground - 40.0);
    const auto near_hole = [&] {
        return std::ranges::any_of(hog::globe::find_pockets(world, up * ground, 100.0, 6.0),
                                   [&](const Pocket& pocket) {
                                       return glm::length(pocket.at - hole) < 6.0;
                                   });
    };
    // Solid rock there before it's dug.
    REQUIRE_FALSE(near_hole());
    hog::globe::dig(world, hole, 6.0);
    CHECK(near_hole());
}
