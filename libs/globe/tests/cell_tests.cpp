#include <hog/globe/cell.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

using hog::DVec3;
using hog::globe::Cell;

namespace {

double angle_between(const DVec3& a, const DVec3& b) {
    return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

bool lists(const hog::globe::CellNeighbors& found, Cell cell) {
    return std::ranges::find(found.all(), cell) != found.all().end();
}

} // namespace

TEST_CASE("a level has six faces of four times the last level's cells", "[globe][cell]") {
    CHECK(hog::globe::cells_at(0) == 6);
    CHECK(hog::globe::cells_at(1) == 24);
    CHECK(hog::globe::cells_at(17) == 6ULL << 34);
    for (std::uint64_t index = 0; index < hog::globe::cells_at(3); ++index) {
        const Cell cell = hog::globe::cell_at_index(3, index);
        REQUIRE(cell.is_valid());
        CHECK(hog::globe::index_of(cell) == index);
    }
}

TEST_CASE("a cell's middle lies under it, down to the finest level", "[globe][cell]") {
    for (const int level : {0, 1, 4, 9, 17, 24}) {
        const std::uint64_t count = hog::globe::cells_at(level);
        for (std::uint64_t k = 0; k < 600; ++k) {
            const Cell cell = hog::globe::cell_at_index(level, (k * 0x9E37'79B9'7F4AULL) % count);
            const DVec3 middle = hog::globe::center(cell);
            CHECK(std::abs(glm::length(middle) - 1.0) < 1e-12);
            CHECK(hog::globe::cell_under(middle, level) == cell);
        }
    }
}

TEST_CASE("cells tile the sphere, none more than root two the size of another", "[globe][cell]") {
    for (const int level : {0, 2, 6}) {
        double total = 0.0;
        double smallest = 1e9;
        double largest = 0.0;
        for (std::uint64_t index = 0; index < hog::globe::cells_at(level); ++index) {
            const double angle = hog::globe::solid_angle(hog::globe::cell_at_index(level, index));
            total += angle;
            smallest = std::min(smallest, angle);
            largest = std::max(largest, angle);
        }
        CHECK(std::abs(total - (4.0 * std::numbers::pi)) < 1e-10);
        CHECK(largest / smallest <= std::numbers::sqrt2 + 1e-9);
        if (level == 6) {
            CHECK(largest / smallest > 1.35);
        }
    }
    const Cell cell = hog::globe::cell_at_index(5, 777);
    const double shell = (1001.0 * 1001.0 * 1001.0) - (1000.0 * 1000.0 * 1000.0);
    CHECK(std::abs(hog::globe::prism_volume(cell, 1000.0, 1001.0) -
                   (hog::globe::solid_angle(cell) * shell / 3.0)) < 1e-9);
}

TEST_CASE("corners turn anticlockwise from outside, and neighbours share two", "[globe][cell]") {
    constexpr int level = 3;
    for (std::uint64_t index = 0; index < hog::globe::cells_at(level); ++index) {
        const Cell cell = hog::globe::cell_at_index(level, index);
        const auto corner = hog::globe::corners(cell);
        const DVec3 middle = hog::globe::center(cell);
        for (std::size_t k = 0; k < 4; ++k) {
            const DVec3& a = corner.at(k);
            const DVec3& b = corner.at((k + 1) % 4);
            CHECK(glm::dot(glm::cross(a - middle, b - middle), middle) > 0.0);
        }
        const auto beside = hog::globe::sides(cell);
        for (const Cell next : beside.all()) {
            const auto theirs = hog::globe::corners(next);
            int shared = 0;
            for (const DVec3& mine : corner) {
                for (const DVec3& other : theirs) {
                    shared += glm::length(mine - other) < 1e-12 ? 1 : 0;
                }
            }
            CHECK(shared == 2);
        }
    }
}

TEST_CASE("every cell has four sides and eight around, seven at the cube's corners",
          "[globe][cell]") {
    for (const int level : {0, 1, 2, 5}) {
        const double step = std::numbers::pi / 2.0 / std::ldexp(1.0, level);
        int corner_cells = 0;
        for (std::uint64_t index = 0; index < hog::globe::cells_at(level); ++index) {
            const Cell cell = hog::globe::cell_at_index(level, index);
            const auto beside = hog::globe::sides(cell);
            REQUIRE(beside.count == 4);
            for (const Cell next : beside.all()) {
                CHECK(next != cell);
                CHECK(lists(hog::globe::sides(next), cell));
                const double apart =
                    angle_between(hog::globe::center(cell), hog::globe::center(next));
                CHECK(apart > 0.6 * step);
                CHECK(apart < 1.3 * step);
            }
            const auto near = hog::globe::around(cell);
            if (level > 0) {
                CHECK((near.count == 8 || near.count == 7));
                corner_cells += near.count == 7 ? 1 : 0;
            }
            for (const Cell next : near.all()) {
                CHECK(next != cell);
                CHECK(lists(hog::globe::around(next), cell));
                CHECK(angle_between(hog::globe::center(cell), hog::globe::center(next)) <
                      1.9 * step);
            }
        }
        if (level > 0) {
            // Three round each of the cube's eight corners.
            CHECK(corner_cells == 24);
        }
    }
}

TEST_CASE("four children tile their parent exactly", "[globe][cell]") {
    for (std::uint64_t index = 0; index < hog::globe::cells_at(4); index += 7) {
        const Cell cell = hog::globe::cell_at_index(4, index);
        double covered = 0.0;
        for (const Cell child : hog::globe::children(cell)) {
            CHECK(hog::globe::parent(child) == cell);
            CHECK(hog::globe::cell_under(hog::globe::center(child), 4) == cell);
            covered += hog::globe::solid_angle(child);
        }
        CHECK(std::abs(covered - hog::globe::solid_angle(cell)) < 1e-12);
    }
}
