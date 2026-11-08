#include "../src/terms.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

using hog::DVec3;
using hog::globe::Cell;
using hog::globe::Rock;

namespace {

struct Setup {
    hog::globe::Planet planet = hog::globe::preset("earth").value_or(hog::globe::Planet{});
    hog::globe::Generator generator{planet};
    hog::globe::terms::Context context{
        .planet = &generator.planet(), .derived = &generator.derived(), .shape = &generator.shape()};
};

// The rocks in cells spread over the planet, with their cells.
std::vector<std::pair<Cell, Rock>> rocks_on(const Setup& setup, int count, double lod) {
    std::vector<std::pair<Cell, Rock>> found;
    const int level = hog::globe::terms::rock_level(setup.generator.planet());
    const std::uint64_t cells = hog::globe::cells_at(level);
    for (std::uint64_t k = 0; k < static_cast<std::uint64_t>(count); ++k) {
        const Cell cell = hog::globe::cell_at_index(level, (k * 2'654'435'761ULL) % cells);
        if (const auto rock = hog::globe::terms::rock_in(setup.context, cell, lod)) {
            found.emplace_back(cell, *rock);
        }
    }
    return found;
}

} // namespace

TEST_CASE("a rock is an ellipsoid with its surface at the half-lengths", "[globe][rockfall]") {
    Rock rock;
    rock.middle = {0.0, 0.0, 1000.0};
    rock.up = {0.0, 0.0, 1.0};
    rock.across = {1.0, 0.0, 0.0};
    rock.axes = {3.0, 2.0, 1.5};
    DVec3 normal{0.0};
    CHECK(rock.distance(rock.middle, normal) < -1.0);
    CHECK(std::abs(rock.distance(rock.middle + DVec3{3.0, 0.0, 0.0}, normal)) < 1e-9);
    CHECK(normal.x > 0.99);
    CHECK(std::abs(rock.distance(rock.middle + DVec3{0.0, 0.0, 1.5}, normal)) < 1e-9);
    CHECK(normal.z > 0.99);
    CHECK(rock.distance(rock.middle + DVec3{0.0, 10.0, 0.0}, normal) > 5.0);
    CHECK(rock.top() == 1001.5);
}

TEST_CASE("rock lies only where the ground is steep, sized and placed inside its cell",
          "[globe][rockfall]") {
    const Setup setup;
    const auto found = rocks_on(setup, 1'500, 2.0);
    REQUIRE_FALSE(found.empty());
    // Fewer than the densest share of cells hold one, as flat ground sheds none.
    CHECK(found.size() < 1'500 * 35 / 100);
    for (const auto& [cell, rock] : found) {
        CHECK(hog::globe::cell_under(rock.up, cell.level) == cell);
        CHECK(rock.axes.x >= 0.8);
        CHECK(rock.axes.x <= 4.5);
        CHECK(rock.axes.z <= rock.axes.x);
        CHECK(hog::globe::properties(rock.material).strength >= 1.0e6);
        CHECK(hog::globe::terms::steepness(setup.context, rock.up) >
              hog::globe::terms::sheds_from);
    }
}

TEST_CASE("coarser detail leaves out the rocks it can't resolve", "[globe][rockfall]") {
    const Setup setup;
    const auto fine = rocks_on(setup, 1'500, 2.0);
    const auto coarse = rocks_on(setup, 1'500, 6.0);
    REQUIRE_FALSE(coarse.empty());
    CHECK(coarse.size() < fine.size());
    for (const auto& [cell, rock] : coarse) {
        CHECK(2.0 * rock.axes.x >= 6.0);
    }
}

TEST_CASE("a rock stands out of the ground its column is cut in", "[globe][rockfall]") {
    const Setup setup;
    const auto found = rocks_on(setup, 1'500, 2.0);
    REQUIRE_FALSE(found.empty());
    const auto& [cell, rock] = found.front();
    const auto column = setup.generator.column(rock.up, 2.0);
    // Reached by its own column, which now rises above the ground to the rock's top.
    REQUIRE(hog::globe::terms::rockfall(setup.context, rock.up, 2.0).has_value());
    CHECK(column.top() > column.ground());
    CHECK(column.top() == rock.top());
    const hog::globe::Sample inside = column.sample(glm::length(rock.middle));
    CHECK(inside.distance < 0.0);
    CHECK(inside.material == rock.material);
    const hog::globe::Sample above = column.sample(rock.top() + 1.0);
    CHECK(above.distance > 0.0);
}

TEST_CASE("rocks over a tile's cells at once are those found one at a time", "[globe][rockfall]") {
    const Setup setup;
    const auto rocks = rocks_on(setup, 4'000, 0.5);
    REQUIRE_FALSE(rocks.empty());
    const int level = hog::globe::terms::rock_level(setup.generator.planet()) + 3;
    const Cell middle = hog::globe::cell_under(rocks.front().second.up, level);
    std::vector<Cell> cells;
    for (std::uint32_t j = 0; j < 32; ++j) {
        for (std::uint32_t i = 0; i < 32; ++i) {
            cells.push_back({.level = static_cast<std::uint8_t>(level),
                             .face = middle.face,
                             .i = middle.i - 16 + i,
                             .j = middle.j - 16 + j});
        }
    }
    const std::vector<std::optional<Rock>> made =
        hog::globe::terms::rockfall(setup.context, cells, 0.5);
    REQUIRE(made.size() == cells.size());
    int found = 0;
    for (std::size_t n = 0; n < cells.size(); ++n) {
        const std::optional<Rock> one =
            hog::globe::terms::rockfall(setup.context, hog::globe::center(cells[n]), 0.5);
        CAPTURE(n);
        REQUIRE(made[n].has_value() == one.has_value());
        if (const std::optional<Rock>& both = made[n]; one && both) {
            CHECK(both->middle == one->middle);
            CHECK(both->axes == one->axes);
            ++found;
        }
    }
    CHECK(found > 0);
}
