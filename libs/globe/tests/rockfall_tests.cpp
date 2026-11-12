#include "../src/joints.hpp"
#include "../src/terms.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

using hog::DVec3;
using hog::globe::Cell;
using hog::globe::Rock;

namespace {

struct Setup {
    hog::globe::Planet planet;
    hog::globe::Generator generator{planet};
    hog::globe::terms::Context context{.planet = &generator.planet(),
                                       .derived = &generator.derived(),
                                       .shape = &generator.shape()};
};

struct Found {
    Cell cell;
    Rock rock;
};

// The rocks in coarsest cells spread over the planet and in the cells inside them.
// Coarse enough that every block is rubble, fine enough that it's still reckoned over rock cells.
constexpr double every_block = 20.0;

std::vector<Found> rocks_on(const Setup& setup, std::uint64_t count, double lod) {
    std::vector<Found> found;
    const int level = hog::globe::terms::rock_level(setup.generator.planet());
    const int finest = level + hog::globe::terms::rock_sizes - 1;
    const std::uint64_t cells = hog::globe::cells_at(level);
    for (std::uint64_t k = 0; k < count; ++k) {
        std::vector<Cell> waiting{hog::globe::cell_at_index(level, (k * 2'654'435'761ULL) % cells)};
        while (!waiting.empty()) {
            const Cell cell = waiting.back();
            waiting.pop_back();
            if (const auto rock = hog::globe::terms::rock_in(setup.context, cell, lod)) {
                found.push_back({.cell = cell, .rock = *rock});
            }
            if (std::cmp_less(cell.level, finest)) {
                for (const Cell inside : hog::globe::children(cell)) {
                    waiting.push_back(inside);
                }
            }
        }
    }
    return found;
}

double width_of(const Setup& setup, Cell cell) {
    const auto corner = hog::globe::corners(cell);
    double width = 1e30;
    for (std::size_t k = 0; k < corner.size(); ++k) {
        width = std::min(width, glm::length(corner[(k + 1) % corner.size()] - corner[k]) *
                                    setup.generator.planet().body.radius);
    }
    return width;
}

} // namespace

TEST_CASE("a rock is a box with its edges knocked off", "[globe][rockfall]") {
    Rock rock;
    rock.middle = {0.0, 0.0, 1000.0};
    rock.up = {0.0, 0.0, 1.0};
    rock.across = {1.0, 0.0, 0.0};
    rock.axes = {3.0, 2.0, 1.5};
    rock.edge = 0.3;
    DVec3 normal{0.0};
    CHECK(rock.distance(rock.middle, normal) == -1.5);
    CHECK(normal.z == 1.0);
    // Flat faces out to their rounded edges.
    CHECK(std::abs(rock.distance(rock.middle + DVec3{3.0, 0.5, 0.2}, normal)) < 1e-9);
    CHECK(normal.x == 1.0);
    CHECK(std::abs(rock.distance(rock.middle + DVec3{0.0, 0.0, 1.5}, normal)) < 1e-9);
    CHECK(normal.z == 1.0);
    // A corner rounded off, standing in from the box's own.
    CHECK(rock.distance(rock.middle + DVec3{3.0, 2.0, 1.5}, normal) > 0.2);
    CHECK(std::abs(rock.distance(rock.middle + DVec3{0.0, 12.0, 0.0}, normal) - 10.0) < 1e-9);
    CHECK(rock.top() == 1001.5);
    CHECK(rock.volume() < 8.0 * 3.0 * 2.0 * 1.5);
    CHECK(rock.volume() > 0.95 * 8.0 * 3.0 * 2.0 * 1.5);
}

TEST_CASE("fallen rock lies only where rock comes loose about it, each in the finest cell it fits",
          "[globe][rockfall]") {
    const Setup setup;
    const auto found = rocks_on(setup, 1'500, 0.5);
    REQUIRE_FALSE(found.empty());
    const int coarsest = hog::globe::terms::rock_level(setup.generator.planet());
    for (const auto& [cell, rock] : found) {
        CHECK(hog::globe::cell_under(glm::normalize(rock.middle), cell.level) == cell);
        CHECK(hog::globe::properties(rock.material).strength >= 1.0e6);
        CHECK(rock.axes.x >= rock.axes.y);
        CHECK(rock.axes.y >= rock.axes.z);
        CHECK(rock.edge < rock.axes.z);
        // Inside the middle of its cell, too wide for those inside it.
        const double across = 2.0 * std::hypot(rock.axes.x, rock.axes.y);
        CHECK(across <= 0.5 * width_of(setup, cell));
        if (cell.level < coarsest + hog::globe::terms::rock_sizes - 1) {
            CHECK(across > 0.25 * width_of(setup, cell));
        }
        Cell holding = cell;
        while (std::cmp_greater(holding.level, coarsest)) {
            holding = hog::globe::parent(holding);
        }
        CHECK(hog::globe::terms::fallen_cover(setup.context, holding) > 0.0);
    }
}

TEST_CASE("fallen blocks lie apart, those in finer cells about the coarser ones",
          "[globe][rockfall]") {
    const Setup setup;
    const auto found = rocks_on(setup, 600, 0.5);
    REQUIRE_FALSE(found.empty());
    int finer = 0;
    for (std::size_t a = 0; a < found.size(); ++a) {
        finer +=
            found[a].cell.level > hog::globe::terms::rock_level(setup.generator.planet()) ? 1 : 0;
        for (std::size_t b = a + 1; b < found.size(); ++b) {
            const Rock& one = found[a].rock;
            const Rock& other = found[b].rock;
            // Footprints apart, as circles round their corners.
            CHECK(glm::length(one.middle - other.middle) >
                  0.9 * (std::hypot(one.axes.x, one.axes.y) +
                         std::hypot(other.axes.x, other.axes.y)));
        }
    }
    CHECK(finer > 0);
}

TEST_CASE("no fallen rock lies where its rubble wouldn't rest", "[globe][rockfall]") {
    const Setup setup;
    const int level = hog::globe::terms::rock_level(setup.generator.planet());
    const std::uint64_t cells = hog::globe::cells_at(level);
    const double rests = std::tan(hog::globe::properties(hog::globe::Material::Gravel).repose);
    int covered = 0;
    int steep = 0;
    for (std::uint64_t k = 0; k < 3'000; ++k) {
        const Cell cell = hog::globe::cell_at_index(level, (k * 2'654'435'761ULL) % cells);
        const double cover = hog::globe::terms::fallen_cover(setup.context, cell);
        CHECK(cover >= 0.0);
        CHECK(cover <= 1.0);
        covered += cover > 0.0 ? 1 : 0;
        const auto ground = hog::globe::terms::ground(setup.context, hog::globe::center(cell),
                                                      width_of(setup, cell) / 2.0);
        if (glm::length(ground.rise) > rests) {
            ++steep;
            CHECK(cover == 0.0);
        }
    }
    CHECK(covered > 10);
    CHECK(steep > 0);
}

TEST_CASE("coarser detail leaves out the rocks it can't resolve", "[globe][rockfall]") {
    const Setup setup;
    const auto fine = rocks_on(setup, 1'500, 0.5);
    const auto coarse = rocks_on(setup, 1'500, 3.0);
    REQUIRE_FALSE(coarse.empty());
    CHECK(coarse.size() < fine.size());
    for (const auto& [cell, rock] : coarse) {
        CHECK(2.0 * rock.axes.x >= 3.0);
    }
}

TEST_CASE("blocks too small for the detail read lie spread over the ground as rubble",
          "[globe][rockfall]") {
    const Setup setup;
    const auto found = rocks_on(setup, 1'500, 0.5);
    REQUIRE_FALSE(found.empty());
    const Rock& rock = found.front().rock;
    const DVec3 down = glm::normalize(rock.middle);
    const Cell coarsest =
        hog::globe::cell_under(down, hog::globe::terms::rock_level(setup.generator.planet()));
    const double cover = hog::globe::terms::fallen_cover(setup.context, coarsest);
    // Read finely enough for every block, none is rubble.
    CHECK(hog::globe::terms::fallen(setup.context, down, 0.01).rubble.depth == 0.0);
    // The coarser, the more of them rubble, all of them at last, of the rock they fell from, no
    // deeper than the tallest block standing over all the ground.
    REQUIRE(cover > 0.0);
    double last = 0.0;
    for (const double lod : {0.5, 1.0, 2.0, 4.0, 8.0, every_block}) {
        const hog::globe::terms::Rubble rubble =
            hog::globe::terms::fallen(setup.context, down, lod).rubble;
        CHECK(rubble.depth >= last);
        CHECK(rubble.depth <= hog::globe::widest_joints);
        if (rubble.depth > 0.0) {
            CHECK(rubble.of == rock.material);
        }
        last = rubble.depth;
    }
    CHECK(last > 0.0);
    // A column read coarsely rises by its rubble, which lies at its top.
    const auto column = setup.generator.column(down, every_block);
    const auto bare = hog::globe::terms::ground(setup.context, down, every_block);
    CHECK(std::abs(column.ground() - (setup.generator.planet().body.radius + bare.height + last)) <
          1e-6);
    CHECK(column.sample(column.ground() - (0.5 * last)).material == rock.material);
}

TEST_CASE("rubble runs on from one coarsest cell to the next without steps", "[globe][rockfall]") {
    const Setup setup;
    const auto found = rocks_on(setup, 1'500, 0.5);
    REQUIRE_FALSE(found.empty());
    const DVec3 down = glm::normalize(found.front().rock.middle);
    const Cell coarsest =
        hog::globe::cell_under(down, hog::globe::terms::rock_level(setup.generator.planet()));
    const auto corner = hog::globe::corners(coarsest);
    // Across two cells, a thousandth of one at a time.
    const DVec3 step = (corner[1] - corner[0]) / 1'000.0;
    std::vector<double> depths;
    for (int k = -1'000; k <= 1'000; ++k) {
        depths.push_back(
            hog::globe::terms::fallen(
                setup.context, glm::normalize(down + (step * static_cast<double>(k))), every_block)
                .rubble.depth);
    }
    const auto [least, most] = std::ranges::minmax(depths);
    REQUIRE(most > least);
    double jump = 0.0;
    for (std::size_t k = 1; k < depths.size(); ++k) {
        jump = std::max(jump, std::abs(depths[k] - depths[k - 1]));
    }
    // Blended linearly, a thousandth of a cell moves it by no more than that of its range.
    CHECK(jump <= 2.0 * (most - least) / 1'000.0);
}

TEST_CASE("a rock stands out of the ground its column is cut in", "[globe][rockfall]") {
    const Setup setup;
    const auto found = rocks_on(setup, 1'500, 0.5);
    REQUIRE_FALSE(found.empty());
    const Rock& rock = found.front().rock;
    const DVec3 down = glm::normalize(rock.middle);
    const auto column = setup.generator.column(down, 0.5);
    // Reached by its own column, which now rises above the ground to the rock's top.
    REQUIRE(hog::globe::terms::rockfall(setup.context, down, 0.5).has_value());
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
    const auto rocks = rocks_on(setup, 1'500, 0.5);
    REQUIRE_FALSE(rocks.empty());
    const int level = hog::globe::terms::rock_level(setup.generator.planet()) + 3;
    const Cell middle = hog::globe::cell_under(glm::normalize(rocks.front().rock.middle), level);
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

TEST_CASE("a coarse tile's cells hold the rock that falls within them", "[globe][rockfall]") {
    const Setup setup;
    const auto rocks = rocks_on(setup, 1'500, 0.5);
    REQUIRE_FALSE(rocks.empty());
    // A tile whose cells are two levels wider than the rock cells.
    const int level = hog::globe::terms::rock_level(setup.generator.planet()) - 2;
    const double lod = hog::globe::voxel_lod(setup.generator) *
                       std::ldexp(1.0, setup.generator.layers().level - level);
    const Cell middle = hog::globe::cell_under(glm::normalize(rocks.front().rock.middle), level);
    std::vector<Cell> cells;
    for (std::uint32_t j = 0; j < 16; ++j) {
        for (std::uint32_t i = 0; i < 16; ++i) {
            cells.push_back({.level = static_cast<std::uint8_t>(level),
                             .face = middle.face,
                             .i = middle.i - 8 + i,
                             .j = middle.j - 8 + j});
        }
    }
    const auto grounds = hog::globe::terms::ground(setup.context, cells, lod);
    const auto alone = hog::globe::terms::fallen(setup.context, cells, lod);
    const auto knowing = hog::globe::terms::fallen(setup.context, cells, lod, cells, grounds);
    REQUIRE(alone.size() == cells.size());
    REQUIRE(knowing.size() == cells.size());
    int covered = 0;
    for (std::size_t n = 0; n < cells.size(); ++n) {
        const hog::globe::terms::Fall one =
            hog::globe::terms::fallen(setup.context, hog::globe::center(cells[n]), lod);
        CAPTURE(n);
        // Too wide a cell for any block to show; the ground already read walks the same way.
        CHECK_FALSE(alone[n].rock.has_value());
        CHECK(knowing[n].rubble.depth == alone[n].rubble.depth);
        CHECK(one.rubble.depth == alone[n].rubble.depth);
        CHECK(alone[n].rubble.depth <= hog::globe::widest_joints);
        covered += alone[n].rubble.depth > 0.0 ? 1 : 0;
    }
    WARN("cells with rubble: " << covered);
}
