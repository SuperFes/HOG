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
#include <tuple>
#include <vector>

using hog::DVec3;
using hog::globe::Generator;
using hog::globe::Pocket;

namespace {

constexpr std::array voxels{1.0, 2.5};

// A small worn planet, made in seconds, at a voxel size.
const Generator& world(double voxel, double wear = 0.03) {
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

TEST_CASE("open space under the ground is flooded as far up as the water table",
          "[globe][pockets]") {
    const double voxel = GENERATE(from_range(voxels));
    // Little wear leaves the passages under the table; much leaves some high on the valley walls.
    const double wear = GENERATE(0.03, 0.3);
    CAPTURE(voxel, wear);
    const Generator& generator = world(voxel, wear);
    const double lod = hog::globe::voxel_lod(generator);
    std::size_t flooded = 0;
    std::size_t dry = 0;
    std::size_t wrong = 0;
    for (std::uint64_t k = 0; k < 4'000; ++k) {
        const DVec3 direction =
            hog::globe::center(hog::globe::cell_at_index(10, (k * 2'654'435'761ULL) % 6'291'456));
        const Generator::Column column = generator.column(direction, lod);
        REQUIRE(column.table() > 0.0);
        for (const auto& [bottom, top] :
             column.caves(column.ground() - 400.0, column.ground(), 0.0)) {
            for (double radius = bottom; radius <= top; radius += lod / 2.0) {
                const hog::globe::Sample sample = column.sample(radius);
                if (sample.distance <= 0.0 || radius >= column.ground()) {
                    continue;
                }
                const double level = std::max(column.table(), column.water());
                // The voxel the surface crosses holds part of one; the rest are all or none.
                if (std::abs(radius - level) < lod) {
                    continue;
                }
                const bool wet = sample.liquid == hog::globe::Material::Water && sample.water > 0.0;
                (wet ? flooded : dry) += 1;
                wrong += wet != (radius < level) ? 1U : 0U;
            }
        }
    }
    std::println("open samples flooded {} dry {}", flooded, dry);
    CHECK(flooded + dry > 100);
    CHECK(wrong == 0);
    CHECK(flooded > 0);
    if (wear > 0.1) {
        CHECK(dry > 0);
    }
}
