#include <hog/sim/cell_grid.hpp>
#include <hog/testing/assertion_guard.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstdint>
#include <numbers>
#include <vector>

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

TEST_CASE("grid cells cover the sphere", "[sim][grid]") {
    const CellGrid grid{4};
    double total = 0.0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        CHECK(grid.area(index) > 0.0);
        total += grid.area(index);
    }
    CHECK_THAT(total, Catch::Matchers::WithinRel(4.0 * std::numbers::pi, 1e-9));
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

TEST_CASE("grids interpolate between cells", "[sim][grid]") {
    const CellGrid grid{4};
    const auto value = [](std::uint32_t index) { return static_cast<double>(index % 7); };
    for (std::uint32_t index = 0; index < grid.size(); index += 37) {
        CHECK_THAT(grid.interpolate(grid.frame(index).up, value),
                   Catch::Matchers::WithinAbs(value(index), 1e-9));
        // Halfway to a neighbour stays within the values around both cells.
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            double low = value(index);
            double high = low;
            for (const std::uint32_t cell : {index, neighbor}) {
                for (const std::uint32_t around : grid.neighbors(cell)) {
                    low = std::min({low, value(cell), value(around)});
                    high = std::max({high, value(cell), value(around)});
                }
            }
            const DVec3 between = glm::normalize(grid.frame(index).up + grid.frame(neighbor).up);
            const double halfway = grid.interpolate(between, value);
            CHECK(halfway >= low - 1e-9);
            CHECK(halfway <= high + 1e-9);
        }
    }
}

TEST_CASE("solving with grid Laplacians recovers a smooth field", "[sim][grid]") {
    // Height along an axis is a first spherical harmonic, whose Laplacian is -2 times itself, so
    // it solves laplacian(u) - u = -3 height. Cells are skewed, so single cells are off by more
    // than the solution is.
    const CellGrid grid{4};
    const DVec3 axis = glm::normalize(DVec3{0.3, -0.5, 0.8});
    const auto height = [&](std::uint32_t index) { return glm::dot(grid.frame(index).up, axis); };
    std::vector<double> solution(grid.size(), 0.0);
    std::vector<double> next(grid.size());
    for (int iteration = 0; iteration < 5'000; ++iteration) {
        for (std::uint32_t index = 0; index < grid.size(); ++index) {
            double around = 0.0;
            double total = 1.0;
            for (std::size_t k = 0; k < 4; ++k) {
                const double weight = grid.laplacian_weights(index)[k];
                around += weight * solution[grid.neighbors(index)[k]];
                total += weight;
            }
            next[index] = ((3.0 * height(index)) + around) / total;
        }
        solution.swap(next);
    }
    double worst = 0.0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        worst = std::max(worst, std::abs(solution[index] - height(index)));
    }
    CHECK(worst < 0.05);
}

TEST_CASE("what a grid Laplacian moves out of a cell moves into its neighbour", "[sim][grid]") {
    const CellGrid grid{3};
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        for (std::size_t k = 0; k < 4; ++k) {
            const std::uint32_t neighbor = grid.neighbors(index)[k];
            const auto back = std::ranges::find(grid.neighbors(neighbor), index) -
                              grid.neighbors(neighbor).begin();
            const double out = grid.area(index) * grid.laplacian_weights(index)[k];
            const double in = grid.area(neighbor) *
                              grid.laplacian_weights(neighbor)[static_cast<std::size_t>(back)];
            CHECK_THAT(out, Catch::Matchers::WithinRel(in, 1e-9));
        }
    }
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
