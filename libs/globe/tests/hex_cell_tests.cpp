#include <hog/globe/hex_cell.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
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
        cells.push_back(hog::globe::cell_at_index(level, index));
    }
    return cells;
}

double angle(DVec3 a, DVec3 b) {
    return std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0));
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
    CHECK(nearest > 0.75 * step);
    CHECK(farthest < 1.25 * step);
}

TEST_CASE("a direction finds the cell whose centre is nearest it", "[globe][hex_cell]") {
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
        const double here = angle(direction, hog::globe::center(found));
        const auto around = hog::globe::neighbors(found);
        for (const HexCell next : around.all()) {
            CHECK(here <= angle(direction, hog::globe::center(next)));
        }
    }
}

TEST_CASE("cells reach the finest level", "[globe][hex_cell]") {
    const DVec3 direction = glm::normalize(DVec3{0.3, -0.5, 0.8});
    const HexCell finest = hog::globe::cell_at(direction, hog::globe::max_level);
    CHECK(finest.is_valid());
    // Finer than a ten-millionth of the radius.
    CHECK(angle(direction, hog::globe::center(finest)) < 1e-7);
}
