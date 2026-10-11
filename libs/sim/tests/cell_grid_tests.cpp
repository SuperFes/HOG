#include <hog/sim/cell_grid.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>

using hog::DVec2;
using hog::DVec3;
using hog::planet::CellId;
using hog::sim::CellGrid;

TEST_CASE("a grid numbers every cell of its level once", "[sim][grid]") {
    const int level = GENERATE(0, 1, 3);
    const CellGrid grid{level};
    CHECK(grid.level() == level);
    CHECK(grid.size() == 6U << (2 * level));
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const CellId cell = grid.cell(index);
        CHECK(cell.level() == level);
        CHECK(grid.index(cell) == index);
    }
}

TEST_CASE("grid neighbours are mutual", "[sim][grid]") {
    const CellGrid grid{3};
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            CHECK(neighbor != index);
            CHECK(std::ranges::count(grid.neighbors(neighbor), index) == 1);
        }
    }
}

TEST_CASE("grid gradients recover a smooth field's slope", "[sim][grid]") {
    // The height of each cell centre along an axis; its slope on the sphere is the axis
    // projected onto the surface.
    const CellGrid grid{5};
    const DVec3 axis = glm::normalize(DVec3{0.3, -0.5, 0.8});
    double worst = 0.0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const auto& frame = grid.frame(index);
        DVec2 gradient{0.0};
        for (std::size_t k = 0; k < 4; ++k) {
            const double difference =
                glm::dot(grid.frame(grid.neighbors(index)[k]).up - frame.up, axis);
            gradient += grid.gradient_weights(index)[k] * difference;
        }
        const DVec2 expected{glm::dot(axis, frame.east), glm::dot(axis, frame.north)};
        worst = std::max(worst, glm::length(gradient - expected));
    }
    CHECK(worst < 0.05);
}

TEST_CASE("grids reject bad input", "[sim][grid]") {
    const hog::testing::AssertionGuard guard;
    using hog::testing::AssertionFailed;
    CHECK_THROWS_AS(CellGrid{-1}, AssertionFailed);
    CHECK_THROWS_AS(CellGrid{CellGrid::max_level + 1}, AssertionFailed);
    const CellGrid grid{2};
    CHECK_THROWS_AS(static_cast<void>(grid.cell(grid.size())), AssertionFailed);
    CHECK_THROWS_AS(static_cast<void>(grid.index(CellId::from_face(0))), AssertionFailed);
}
