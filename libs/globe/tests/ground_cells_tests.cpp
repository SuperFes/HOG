#include "../src/ground_cells.hpp"
#include "../src/terms.hpp"

#include <hog/core/math.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

using hog::globe::Cell;
using hog::globe::GroundCells;
namespace terms = hog::globe::terms;

namespace {

terms::Context context_of(const hog::globe::Generator& generator,
                          const GroundCells* cells = nullptr) {
    return {.planet = &generator.planet(),
            .derived = &generator.derived(),
            .shape = &generator.shape(),
            .cells = cells};
}

// A square of cells somewhere in the test world.
std::vector<Cell> some_cells(int level) {
    const Cell middle = hog::globe::cell_under(glm::normalize(hog::DVec3{0.3, 0.5, 0.8}), level);
    std::vector<Cell> cells;
    for (std::uint32_t j = 0; j < 8; ++j) {
        for (std::uint32_t i = 0; i < 8; ++i) {
            cells.push_back({.level = middle.level,
                             .face = middle.face,
                             .i = middle.i - 4 + i,
                             .j = middle.j - 4 + j});
        }
    }
    return cells;
}

void check_same(const terms::Ground& kept, const terms::Ground& fresh) {
    CHECK(kept.height == fresh.height);
    CHECK(kept.rise == fresh.rise);
    CHECK(kept.lake == fresh.lake);
    CHECK(kept.river == fresh.river);
    CHECK(kept.strength == fresh.strength);
    CHECK(kept.layers.topsoil == fresh.layers.topsoil);
}

} // namespace

TEST_CASE("a cell's ground kept is the ground read afresh", "[globe][ground_cells]") {
    const auto& generator = hog::testing::small_world();
    const GroundCells cells;
    const int level = generator.layers().level - 3;
    const double lod = terms::cell_lod(generator.planet(), level);
    const std::vector<Cell> square = some_cells(level);
    const std::vector<terms::Ground> fresh = terms::ground(context_of(generator), square, lod);
    const std::vector<terms::Ground> first =
        terms::ground(context_of(generator, &cells), square, lod);
    CHECK(cells.size() == square.size());
    const std::vector<terms::Ground> again =
        terms::ground(context_of(generator, &cells), square, lod);
    CHECK(cells.size() == square.size());
    for (std::size_t n = 0; n < square.size(); ++n) {
        CAPTURE(n);
        check_same(first[n], fresh[n]);
        check_same(again[n], fresh[n]);
    }
    // Read at another detail, or with crags placed apart, is another ground, kept beside it.
    (void)terms::ground(context_of(generator, &cells), square.front(), lod * 2.0);
    terms::Context apart = context_of(generator, &cells);
    apart.crags = false;
    (void)terms::ground(apart, square.front(), lod);
    CHECK(cells.size() == square.size() + 2);
}

TEST_CASE("cells read on many threads at once are each read alike", "[globe][ground_cells]") {
    const auto& generator = hog::testing::small_world();
    const GroundCells cells;
    const int level = generator.layers().level - 2;
    const double lod = terms::cell_lod(generator.planet(), level);
    const std::vector<Cell> square = some_cells(level);
    const std::vector<terms::Ground> fresh = terms::ground(context_of(generator), square, lod);
    std::vector<std::vector<terms::Ground>> read(4);
    {
        std::vector<std::thread> threads;
        threads.reserve(read.size());
        for (std::vector<terms::Ground>& each : read) {
            threads.emplace_back(
                [&] { each = terms::ground(context_of(generator, &cells), square, lod); });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }
    for (const std::vector<terms::Ground>& each : read) {
        REQUIRE(each.size() == square.size());
        for (std::size_t n = 0; n < square.size(); ++n) {
            check_same(each[n], fresh[n]);
        }
    }
    CHECK(cells.size() == square.size());
}

TEST_CASE("cells keep about as many as they're told, the latest read", "[globe][ground_cells]") {
    const GroundCells cells{256};
    const auto key = [](std::uint32_t n) {
        return GroundCells::Key{.cell = {.level = 20, .face = 1, .i = n, .j = 7}, .lod = 1.0};
    };
    for (std::uint32_t n = 0; n < 10'000; ++n) {
        terms::Ground ground;
        ground.height = static_cast<double>(n);
        cells.keep(key(n), ground);
    }
    CHECK(cells.size() >= 256);
    CHECK(cells.size() <= 512);
    CHECK_FALSE(cells.find(key(0)).has_value());
    const std::optional<terms::Ground> last = cells.find(key(9'999));
    REQUIRE(last.has_value());
    CHECK(last.value_or(terms::Ground{}).height == 9'999.0);
}

TEST_CASE("the cells' detail follows their width down to two voxels", "[globe][ground_cells]") {
    const auto& generator = hog::testing::small_world();
    const hog::globe::Planet& planet = generator.planet();
    const int voxels = generator.layers().level;
    CHECK(terms::cell_lod(planet, voxels) == 2.0 * planet.voxel);
    CHECK(terms::cell_lod(planet, voxels - 1) == 2.0 * planet.voxel);
    CHECK(terms::cell_lod(planet, voxels - 4) == 16.0 * planet.voxel);
    CHECK(terms::cell_lod(planet, voxels + 2) == 2.0 * planet.voxel);
    // And back: the level read with a detail.
    for (int level = 1; level < voxels; ++level) {
        CHECK(terms::cell_level(planet, terms::cell_lod(planet, level)) == level);
    }
    CHECK(terms::cell_level(planet, 0.1 * planet.voxel) == voxels);
    CHECK(terms::cell_level(planet, 1e12) == 0);
}
