#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/pockets.hpp>

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
#include <vector>

using hog::DVec3;
using hog::globe::Generator;
using hog::globe::Pocket;

namespace {

constexpr std::array voxels{1.0, 2.5};

// A small worn planet, made in seconds, at a voxel size.
const Generator& world(double voxel) {
    static std::vector<std::pair<double, Generator>> made;
    for (const auto& [size, generator] : made) {
        if (size == voxel) {
            return generator;
        }
    }
    auto planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
    planet.radius = 4'000.0;
    planet.floor_depth = 100.0;
    planet.sky_height = 500.0;
    planet.wear = 0.03;
    planet.voxel = voxel;
    made.emplace_back(voxel, Generator{planet});
    return made.back().second;
}

} // namespace

TEST_CASE("the pockets found under the ground are open air, near to far", "[globe][pockets]") {
    const double voxel = GENERATE(from_range(voxels));
    CAPTURE(voxel);
    const Generator& generator = world(voxel);
    const double lod = hog::globe::voxel_lod(generator);
    constexpr double range = 300.0;
    std::size_t found = 0;
    std::size_t open = 0;
    for (std::uint64_t k = 0; k < 40; ++k) {
        const DVec3 eye =
            hog::globe::center(hog::globe::cell_at_index(7, (k * 2'654'435'761ULL) % 98'304)) *
            generator.planet().radius;
        const auto started = std::chrono::steady_clock::now();
        const std::vector<Pocket> pockets = hog::globe::find_pockets(generator, eye, range, 6.0);
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
            // Open air lies up or down the column the pocket is at, within its own height.
            const Generator::Column column = generator.column(glm::normalize(pocket.at), lod);
            const double middle = glm::length(pocket.at);
            for (double radius = middle - pocket.height; radius <= middle + pocket.height;
                 radius += lod / 2.0) {
                if (column.sample(radius).distance > 0.0 && radius < column.ground()) {
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
