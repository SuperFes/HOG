#include <hog/core/hash.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <utility>
#include <vector>

using hog::planet::LevelGrid;

namespace {

constexpr double radius = 115'000.0;

} // namespace

TEST_CASE("a level grid numbers every cell once", "[planet][level_grid]") {
    const LevelGrid grid{4, radius};
    REQUIRE(grid.size() == 6U * 16U * 16U);
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        REQUIRE(grid.cell(index).level() == 4);
        REQUIRE(grid.index(grid.cell(index)) == index);
    }
}

TEST_CASE("level grid neighbours are mutual, eight or seven at cube corners",
          "[planet][level_grid]") {
    const LevelGrid grid{3, radius};
    int corners = 0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const auto around = grid.neighbors(index);
        REQUIRE((around.size() == 8 || around.size() == 7));
        corners += around.size() == 7 ? 1 : 0;
        for (const std::uint32_t next : around) {
            REQUIRE(next != index);
            REQUIRE(std::ranges::count(grid.neighbors(next), index) == 1);
        }
    }
    CHECK(corners == 8 * 3);
}

TEST_CASE("level grid distances are the same both ways, about a cell apart",
          "[planet][level_grid]") {
    const LevelGrid grid{5, radius};
    const double edge = 2.0 * std::numbers::pi * radius / (4.0 * 32.0);
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const auto around = grid.neighbors(index);
        for (std::size_t k = 0; k < around.size(); ++k) {
            const auto back = grid.neighbors(around[k]);
            const auto at = std::ranges::find(back, index) - back.begin();
            REQUIRE(grid.distance(around[k], static_cast<std::size_t>(at)) ==
                    grid.distance(index, k));
            const double metres = hog::planet::fixed_to_double(grid.distance(index, k));
            REQUIRE(metres > 0.5 * edge);
            REQUIRE(metres < 2.0 * edge);
        }
    }
}

TEST_CASE("level grid distances hold for cells of any size", "[planet][level_grid]") {
    // A face's cells on an Earth-sized planet, thousands of kilometres apart.
    const double earth = 6'371'000.0;
    const LevelGrid grid{1, earth};
    const double edge = 2.0 * std::numbers::pi * earth / 8.0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        for (std::size_t k = 0; k < grid.neighbors(index).size(); ++k) {
            const double metres = hog::planet::fixed_to_double(grid.distance(index, k));
            REQUIRE(metres > 0.5 * edge);
            REQUIRE(metres < 2.0 * edge);
        }
    }
}

TEST_CASE("level grid areas cover the sphere", "[planet][level_grid]") {
    const LevelGrid grid{6, radius};
    double total = 0.0;
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        REQUIRE(grid.area(index) > 0);
        total += static_cast<double>(grid.area(index));
    }
    CHECK(total == Catch::Approx(4.0 * std::numbers::pi * radius * radius).epsilon(0.01));
}

TEST_CASE("a sampled field is each grid cell's own at its centre and between its neighbours'",
          "[planet][level_grid]") {
    const LevelGrid grid{3, radius};
    std::vector<std::int64_t> field(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        field[index] = static_cast<std::int64_t>(hog::mix64(index) % 1'000'000);
    }
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const hog::planet::CellId cell = grid.cell(index);
        REQUIRE(grid.sample(field, cell) == field[index]);
        std::int64_t lowest = field[index];
        std::int64_t highest = field[index];
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            lowest = std::min(lowest, field[neighbor]);
            highest = std::max(highest, field[neighbor]);
        }
        for (int child = 0; child < 4; ++child) {
            // Down to leaves, so the sample works in the finest cells' units.
            hog::planet::CellId inside = cell.child(child);
            while (!inside.is_leaf()) {
                inside = inside.child(3 - child);
            }
            const std::int64_t value = grid.sample(field, inside);
            REQUIRE(value >= lowest);
            REQUIRE(value <= highest);
        }
    }
}

TEST_CASE("a sample blends only the four grid cells around a point", "[planet][level_grid]") {
    const LevelGrid grid{3, radius};
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const hog::planet::CellId cell = grid.cell(index);
        for (int child = 0; child < 4; ++child) {
            // Towards the cell's corner, where every corner weighs alike.
            hog::planet::CellId inside = cell.child(child);
            while (!inside.is_leaf()) {
                inside = inside.child(child);
            }
            const auto corners = grid.corners(inside);
            REQUIRE(corners[0] == index);
            // Along u and v from the cell, whether inside a face or across its edge.
            const auto edges = hog::planet::edge_neighbors(cell);
            // Which side of the cell's centre the point lies on, along u and along v.
            const auto leaf = inside.to_face_ij();
            const int below = leaf.level - cell.level() - 1;
            const std::size_t quadrant = ((leaf.i >> below) & 1U) + (2 * ((leaf.j >> below) & 1U));
            REQUIRE(grid.cell(corners[1]) == edges.at(quadrant % 2));
            REQUIRE(grid.cell(corners[2]) == edges.at(2 + (quadrant / 2)));
            // Each corner touches every other, so a blend never reaches past its neighbours.
            for (const std::uint32_t corner : corners) {
                for (const std::uint32_t other : corners) {
                    REQUIRE(
                        (corner == other || std::ranges::contains(grid.neighbors(corner), other)));
                }
            }
            for (const std::uint32_t one : corners) {
                std::vector<std::int64_t> field(grid.size(), 0);
                field[one] = hog::planet::fixed_one;
                REQUIRE(grid.sample(field, inside) > 0);
            }
            for (const std::uint32_t other : grid.neighbors(index)) {
                if (!std::ranges::contains(corners, other)) {
                    std::vector<std::int64_t> field(grid.size(), 0);
                    field[other] = hog::planet::fixed_one;
                    REQUIRE(grid.sample(field, inside) == 0);
                }
            }
        }
    }
}

namespace {

// Probes this many levels below the grid's cells.
constexpr int finer = 6;

std::vector<std::int64_t> scattered(const LevelGrid& grid) {
    std::vector<std::int64_t> field(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        field[index] = static_cast<std::int64_t>(hog::mix64(index) % 1'000'000);
    }
    return field;
}

} // namespace

TEST_CASE("a smooth sample keeps a level field level and stays within the cells around a point",
          "[planet][level_grid]") {
    const LevelGrid grid{3, radius};
    const std::vector<std::int64_t> level(grid.size(), 12'345);
    const std::vector<std::int64_t> field = scattered(grid);
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const hog::planet::CellId cell = grid.cell(index);
        std::int64_t lowest = field[index];
        std::int64_t highest = field[index];
        for (const std::uint32_t neighbor : grid.neighbors(index)) {
            lowest = std::min(lowest, field[neighbor]);
            highest = std::max(highest, field[neighbor]);
        }
        REQUIRE(grid.smooth(level, cell) == 12'345);
        for (int child = 0; child < 4; ++child) {
            hog::planet::CellId inside = cell.child(child);
            while (!inside.is_leaf()) {
                inside = inside.child(child);
            }
            REQUIRE(grid.smooth(level, inside) == 12'345);
            const std::int64_t value = grid.smooth(field, inside);
            REQUIRE(value >= lowest);
            REQUIRE(value <= highest);
        }
    }
}

TEST_CASE("a smooth sample follows a sloping field", "[planet][level_grid]") {
    const LevelGrid grid{3, radius};
    const std::uint32_t side = 1U << 3U;
    std::vector<std::int64_t> field(grid.size(), 0);
    for (std::uint32_t j = 0; j < side; ++j) {
        for (std::uint32_t i = 0; i < side; ++i) {
            field[grid.index(hog::planet::CellId::from_face_ij(0, i, j, 3))] =
                (1'000 * std::int64_t{i}) + (300 * std::int64_t{j});
        }
    }
    // Away from the face's edge, so all nine cells lie on the slope.
    const std::uint32_t fine = 1U << static_cast<unsigned>(finer);
    for (std::uint32_t y = fine + (fine / 2); y < ((side - 1) * fine) - (fine / 2); ++y) {
        for (std::uint32_t x = fine + (fine / 2); x < ((side - 1) * fine) - (fine / 2); ++x) {
            const double u = ((x + 0.5) / fine) - 0.5;
            const double v = ((y + 0.5) / fine) - 0.5;
            const auto value = static_cast<double>(
                grid.smooth(field, hog::planet::CellId::from_face_ij(0, x, y, 3 + finer)));
            REQUIRE(value == Catch::Approx((1'000 * u) + (300 * v)).margin(2.0));
        }
    }
}

TEST_CASE("a smooth sample has no steps or creases along cells' edges", "[planet][level_grid]") {
    const LevelGrid grid{3, radius};
    const std::vector<std::int64_t> field = scattered(grid);
    const int level = 3 + finer;
    const std::uint32_t side = 1U << static_cast<unsigned>(level);
    const auto at = [&](int face, std::uint32_t i, std::uint32_t j) {
        return grid.smooth(field, hog::planet::CellId::from_face_ij(face, i, j, level));
    };
    // Neighbouring cells differ by up to a million, a sixty-fourth of that a probe apart; a
    // crease would turn the slope by as much again within one probe.
    std::int64_t steepest = 0;
    std::int64_t sharpest = 0;
    for (int face = 0; face < 6; ++face) {
        for (std::uint32_t row = 0; row < side; ++row) {
            for (std::uint32_t k = 1; k + 1 < side; ++k) {
                const std::array along_u{at(face, k - 1, row), at(face, k, row),
                                         at(face, k + 1, row)};
                const std::array along_v{at(face, row, k - 1), at(face, row, k),
                                         at(face, row, k + 1)};
                for (const auto& line : {along_u, along_v}) {
                    steepest = std::max(steepest, std::abs(line[1] - line[0]));
                    sharpest = std::max(sharpest, std::abs(line[2] - (2 * line[1]) + line[0]));
                }
            }
        }
        // Across the face's edges too.
        for (std::uint32_t k = 0; k < side; ++k) {
            for (const auto& [i, j] : {std::pair{0U, k}, std::pair{side - 1, k}, std::pair{k, 0U},
                                       std::pair{k, side - 1}}) {
                const hog::planet::CellId probe =
                    hog::planet::CellId::from_face_ij(face, i, j, level);
                for (const hog::planet::CellId across : hog::planet::edge_neighbors(probe)) {
                    steepest = std::max(
                        steepest, std::abs(grid.smooth(field, across) - grid.smooth(field, probe)));
                }
            }
        }
    }
    CHECK(steepest < 2'000'000 / 64);
    CHECK(sharpest < 1'000);
}
