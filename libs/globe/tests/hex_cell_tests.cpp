#include <hog/globe/hex_cell.hpp>
#include <hog/globe/hex_grid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <random>
#include <set>
#include <vector>

using hog::DVec3;
using hog::globe::HexCell;

namespace {

std::vector<HexCell> all_cells(int level) {
    std::vector<HexCell> cells;
    cells.reserve(hog::globe::cell_count(level));
    for (std::uint64_t index = 0; index < hog::globe::cell_count(level); ++index) {
        cells.push_back(hog::globe::hex_cell_at_index(level, index));
    }
    return cells;
}

// Precise for small angles too, as acos is not.
double angle(DVec3 a, DVec3 b) {
    return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

} // namespace

TEST_CASE("a level holds ten diamonds of cells and the two poles", "[globe][hex_cell]") {
    CHECK(hog::globe::cell_count(0) == 12);
    CHECK(hog::globe::cell_count(1) == 42);
    CHECK(hog::globe::cell_count(3) == 642);
    for (int level = 0; level <= 4; ++level) {
        std::uint64_t index = 0;
        for (const HexCell cell : all_cells(level)) {
            REQUIRE(cell.is_valid());
            CHECK(hog::globe::index_of(cell) == index++);
        }
    }
}

TEST_CASE("the twelve corners of the icosahedron are pentagons and every other cell a hexagon",
          "[globe][hex_cell]") {
    for (int level = 0; level <= 4; ++level) {
        int pentagons = 0;
        for (const HexCell cell : all_cells(level)) {
            const auto sides = hog::globe::neighbors(cell).count;
            CHECK(sides == (hog::globe::is_pentagon(cell) ? 5 : 6));
            pentagons += hog::globe::is_pentagon(cell) ? 1 : 0;
        }
        CHECK(pentagons == 12);
    }
}

TEST_CASE("neighbours are mutual, distinct and tile the sphere in triangles", "[globe][hex_cell]") {
    for (int level = 0; level <= 4; ++level) {
        std::uint64_t sides = 0;
        for (const HexCell cell : all_cells(level)) {
            const auto around = hog::globe::neighbors(cell);
            const std::set<HexCell> distinct(around.all().begin(), around.all().end());
            CHECK(distinct.size() == around.count);
            CHECK(!distinct.contains(cell));
            for (const HexCell next : around.all()) {
                REQUIRE(next.is_valid());
                const auto back = hog::globe::neighbors(next);
                CHECK(std::ranges::find(back.all(), cell) != back.all().end());
            }
            sides += around.count;
        }
        // A sphere cut into triangles has three edges for each vertex, less six.
        CHECK(sides / 2 == (3 * hog::globe::cell_count(level)) - 6);
    }
}

TEST_CASE("neighbours stand about one step apart all over the sphere", "[globe][hex_cell]") {
    constexpr int level = 5;
    double nearest = 10.0;
    double farthest = 0.0;
    for (const HexCell cell : all_cells(level)) {
        const auto around = hog::globe::neighbors(cell);
        for (const HexCell next : around.all()) {
            const double apart = angle(hog::globe::center(cell), hog::globe::center(next));
            nearest = std::min(nearest, apart);
            farthest = std::max(farthest, apart);
        }
    }
    // The icosahedron's edge is a little over 63°, split 32 times.
    const double step = std::atan(2.0) / 32.0;
    // Keeping areas equal stretches the cells a little by the icosahedron's corners.
    CHECK(nearest > 0.9 * step);
    CHECK(farthest < 1.25 * step);
}

TEST_CASE("a direction finds the cell about it, and a centre its own cell", "[globe][hex_cell]") {
    for (int level = 0; level <= 4; ++level) {
        for (const HexCell cell : all_cells(level)) {
            CHECK(hog::globe::cell_at(hog::globe::center(cell), level) == cell);
        }
    }
    std::mt19937_64 random{7};
    std::normal_distribution<double> spread;
    for (int sample = 0; sample < 2000; ++sample) {
        const DVec3 direction =
            glm::normalize(DVec3{spread(random), spread(random), spread(random)});
        const int level = sample % 12;
        const HexCell found = hog::globe::cell_at(direction, level);
        // Within a hexagon's reach of its centre, give or take the projection's stretch.
        const double step = std::atan(2.0) / std::ldexp(1.0, level);
        CHECK(angle(direction, hog::globe::center(found)) < 0.8 * step);
    }
}

TEST_CASE("every hexagon holds an equal share of the sphere", "[globe][hex_cell]") {
    // Directions spread evenly over the sphere land in each cell as often as its area says.
    constexpr int level = 2;
    constexpr int samples = 2'000'000;
    std::vector<int> landed(hog::globe::cell_count(level), 0);
    std::mt19937_64 random{11};
    std::normal_distribution<double> spread;
    for (int sample = 0; sample < samples; ++sample) {
        const DVec3 direction =
            glm::normalize(DVec3{spread(random), spread(random), spread(random)});
        ++landed[hog::globe::index_of(hog::globe::cell_at(direction, level))];
    }
    double total = 0.0;
    for (const HexCell cell : all_cells(level)) {
        const double expected = samples * hog::globe::solid_angle(cell) / (4.0 * std::numbers::pi);
        CHECK(std::abs(landed[hog::globe::index_of(cell)] - expected) < 0.03 * expected);
        total += hog::globe::solid_angle(cell);
    }
    CHECK(std::abs(total - (4.0 * std::numbers::pi)) < 1e-12);
}

TEST_CASE("a cell's corners ring it anticlockwise, each between two neighbours",
          "[globe][hex_cell]") {
    constexpr int level = 5;
    double outlined = 0.0;
    for (const HexCell cell : all_cells(level)) {
        const auto ring = hog::globe::ring(cell);
        REQUIRE(ring.count == hog::globe::neighbors(cell).count);
        const DVec3 up = hog::globe::center(cell);
        const auto corners = ring.outline();
        const auto around = ring.around();
        double area = 0.0;
        for (std::size_t k = 0; k < corners.size(); ++k) {
            const DVec3 a = corners[k];
            const DVec3 b = corners[(k + 1) % corners.size()];
            // Anticlockwise about the outward centre.
            CHECK(glm::dot(glm::cross(a - up, b - up), up) > 0.0);
            // A corner stands about as far from the cell as from the two neighbours it lies
            // between.
            const double here = angle(a, up);
            CHECK(std::abs(angle(a, hog::globe::center(around[k])) - here) < 0.35 * here);
            CHECK(std::abs(angle(a, hog::globe::center(around[(k + 1) % around.size()])) - here) <
                  0.35 * here);
            // The fan's triangle from the centre, by its spherical excess.
            area += 2.0 * std::atan2(std::abs(glm::dot(up, glm::cross(a, b))),
                                     1.0 + glm::dot(up, a) + glm::dot(a, b) + glm::dot(b, up));
        }
        // The corners' straight-sided outline covers the cell's area, less its curved sides.
        CHECK(std::abs(area - hog::globe::solid_angle(cell)) <
              0.05 * hog::globe::solid_angle(cell));
        outlined += area;
    }
    CHECK(std::abs(outlined - (4.0 * std::numbers::pi)) < 1e-3);
}

TEST_CASE("prisms over every cell fill the shell between two radii", "[globe][hex_cell]") {
    constexpr int level = 3;
    double filled = 0.0;
    for (const HexCell cell : all_cells(level)) {
        filled += hog::globe::prism_volume(cell, 6'000.0, 6'100.0);
    }
    const double shell = 4.0 / 3.0 * std::numbers::pi *
                         ((6'100.0 * 6'100.0 * 6'100.0) - (6'000.0 * 6'000.0 * 6'000.0));
    CHECK(std::abs(filled - shell) < 1e-9 * shell);
}

TEST_CASE("cells reach the finest level", "[globe][hex_cell]") {
    const DVec3 direction = glm::normalize(DVec3{0.3, -0.5, 0.8});
    const HexCell finest = hog::globe::cell_at(direction, hog::globe::max_level);
    CHECK(finest.is_valid());
    // Finer than a ten-millionth of the radius.
    CHECK(angle(direction, hog::globe::center(finest)) < 1e-7);
    std::mt19937_64 random{3};
    std::uniform_int_distribution<std::uint64_t> pick(
        0, hog::globe::cell_count(hog::globe::max_level) - 1);
    for (int sample = 0; sample < 2000; ++sample) {
        const HexCell cell = hog::globe::hex_cell_at_index(hog::globe::max_level, pick(random));
        CHECK(hog::globe::cell_at(hog::globe::center(cell), hog::globe::max_level) == cell);
    }
}

TEST_CASE("a coarse cell hands its centre whole and the cells halfway to its neighbours half",
          "[globe][hex_cell]") {
    for (int level = 0; level <= 4; ++level) {
        std::vector<int> halves(hog::globe::cell_count(level + 1), 0);
        for (const HexCell cell : all_cells(level)) {
            const auto under = hog::globe::children(cell);
            CHECK(under.count == hog::globe::neighbors(cell).count + 1);
            // The centre stays where it was.
            CHECK(angle(hog::globe::center(under.cells[0]), hog::globe::center(cell)) < 1e-12);
            for (std::size_t k = 0; k < under.count; ++k) {
                halves[hog::globe::index_of(under.cells[k])] += under.halves[k];
                // The fine cell names the coarse one back, by as many halves.
                const auto over = hog::globe::parents(under.cells[k]);
                const auto back = std::ranges::find(over.all(), cell);
                REQUIRE(back != over.all().end());
                CHECK(over.halves[static_cast<std::size_t>(back - over.all().begin())] ==
                      under.halves[k]);
            }
        }
        // Every fine cell is shared out whole, once.
        CHECK(std::ranges::all_of(halves, [](int each) { return each == 2; }));
    }
}

TEST_CASE("moving amounts a level down and back up keeps every unit", "[globe][hex_cell]") {
    constexpr int level = 3;
    std::mt19937_64 random{5};
    std::uniform_int_distribution<std::int64_t> amount(-1'000'003, 1'000'003);
    std::vector<std::int64_t> coarse(hog::globe::cell_count(level));
    for (std::int64_t& each : coarse) {
        each = amount(random);
    }
    const std::int64_t total = std::accumulate(coarse.begin(), coarse.end(), std::int64_t{0});

    std::vector<std::int64_t> fine(hog::globe::cell_count(level + 1), 0);
    for (const HexCell cell : all_cells(level)) {
        const auto under = hog::globe::children(cell);
        const auto dealt = hog::globe::share_out(coarse[hog::globe::index_of(cell)], under);
        for (std::size_t k = 0; k < under.count; ++k) {
            fine[hog::globe::index_of(under.cells[k])] += dealt[k];
        }
    }
    CHECK(std::accumulate(fine.begin(), fine.end(), std::int64_t{0}) == total);

    std::vector<std::int64_t> back(coarse.size(), 0);
    for (const HexCell cell : all_cells(level + 1)) {
        const auto over = hog::globe::parents(cell);
        const auto dealt = hog::globe::share_out(fine[hog::globe::index_of(cell)], over);
        for (std::size_t k = 0; k < over.count; ++k) {
            back[hog::globe::index_of(over.cells[k])] += dealt[k];
        }
    }
    CHECK(std::accumulate(back.begin(), back.end(), std::int64_t{0}) == total);
}

TEST_CASE("an amount is dealt out by halves in whole units", "[globe][hex_cell]") {
    hog::globe::HexShares shares;
    shares.count = 3;
    shares.halves = {2, 1, 1};
    CHECK(hog::globe::share_out(8, shares) == std::array<std::int64_t, 7>{4, 2, 2});
    CHECK(hog::globe::share_out(11, shares) == std::array<std::int64_t, 7>{5, 3, 3});
    CHECK(hog::globe::share_out(-5, shares) == std::array<std::int64_t, 7>{-3, -1, -1});
}

TEST_CASE("a grid holds every cell's neighbours by index", "[globe][hex_grid]") {
    const hog::globe::HexGrid grid{3};
    REQUIRE(grid.size() == hog::globe::cell_count(3));
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const auto around = hog::globe::neighbors(hog::globe::hex_cell_at_index(3, index));
        REQUIRE(grid.neighbors(index).size() == around.count);
        for (std::size_t k = 0; k < around.count; ++k) {
            CHECK(grid.neighbors(index)[k] == hog::globe::index_of(around.cells[k]));
        }
    }
}
