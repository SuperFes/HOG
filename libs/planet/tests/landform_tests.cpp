#include <hog/planet/landform.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <ranges>
#include <vector>

using hog::planet::CellId;
using hog::planet::fixed_one;
using hog::planet::Landform;
using hog::planet::LevelGrid;

namespace {

constexpr int level = 4;

// A slope worn for a while, so the change differs from cell to cell.
const Landform& landform() {
    static const Landform landform = [] {
        LevelGrid grid{level, 115'000.0};
        std::vector<std::int64_t> heights;
        for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
            const hog::DVec3 at = hog::planet::cell_center(grid.cell(cell));
            const double metres = at.z > -0.3
                                      ? 50.0 + (800.0 * (at.z + 0.3)) +
                                            (60.0 * std::sin(17.0 * at.x) * std::cos(13.0 * at.y))
                                      : -1000.0;
            heights.push_back(std::llround(metres * static_cast<double>(fixed_one)));
        }
        const std::vector<std::int64_t> uplift(grid.size(), 10 * fixed_one);
        const std::vector<std::int64_t> erodibility(grid.size(), fixed_one / 20);
        const std::vector<std::int64_t> runoff(grid.size(), fixed_one / 2);
        return Landform{std::move(grid),
                        std::move(heights),
                        {.uplift = uplift, .erodibility = erodibility, .runoff = runoff},
                        hog::planet::Hillslope{},
                        30};
    }();
    return landform;
}

std::int64_t range() {
    const auto change = landform().change();
    return std::ranges::max(change) - std::ranges::min(change);
}

} // namespace

TEST_CASE("a landform's change at its own cells lies among the cell's and its neighbours'",
          "[planet][landform]") {
    REQUIRE(range() > 100 * fixed_one);
    const auto change = landform().change();
    for (std::uint32_t cell = 0; cell < landform().grid().size(); ++cell) {
        std::int64_t lowest = change[cell];
        std::int64_t highest = change[cell];
        for (const std::uint32_t around : landform().grid().neighbors(cell)) {
            lowest = std::min(lowest, change[around]);
            highest = std::max(highest, change[around]);
        }
        const std::int64_t value = landform().change(landform().grid().cell(cell));
        REQUIRE(value >= lowest);
        REQUIRE(value <= highest);
    }
}

TEST_CASE("a landform's change makes no new highs or lows", "[planet][landform]") {
    const auto change = landform().change();
    const auto [lowest, highest] = std::ranges::minmax(change);
    std::mt19937_64 random{7};
    std::uniform_int_distribution<std::uint32_t> position{0, (1U << 12) - 1};
    for (int sample = 0; sample < 20'000; ++sample) {
        const CellId cell =
            CellId::from_face_ij(sample % 6, position(random), position(random), 12);
        const std::int64_t value = landform().change(cell);
        REQUIRE(value >= lowest);
        REQUIRE(value <= highest);
    }
}

TEST_CASE("a landform's change runs on smoothly between cells, across faces too",
          "[planet][landform]") {
    // Fine cells are 1/64 of a grid cell, so neighbours should differ by a few hundredths of the
    // range at most; a seam would show as a jump of a good share of it.
    constexpr int fine = level + 6;
    const std::uint32_t side = 1U << fine;
    std::int64_t largest = 0;
    for (int face = 0; face < 6; ++face) {
        for (std::uint32_t i = 0; i < side; ++i) {
            for (const std::uint32_t j : {0U, 1U, side / 3, (side / 2) - 1, side / 2, side - 1}) {
                for (const CellId cell : {CellId::from_face_ij(face, i, j, fine),
                                          CellId::from_face_ij(face, j, i, fine)}) {
                    for (const CellId next : hog::planet::edge_neighbors(cell)) {
                        largest = std::max(
                            largest, std::abs(landform().change(cell) - landform().change(next)));
                    }
                }
            }
        }
    }
    CHECK(largest < range() / 16);
}

TEST_CASE("worn with its slopes, land never stands steeper than its rock, and soil gathers",
          "[planet][landform]") {
    LevelGrid grid{5, 115'000.0};
    std::vector<std::int64_t> heights;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const hog::DVec3 at = hog::planet::cell_center(grid.cell(cell));
        const double metres = at.z > -0.3
                                  ? 50.0 + (3'000.0 * (at.z + 0.3)) +
                                        (400.0 * std::sin(37.0 * at.x) * std::cos(29.0 * at.y))
                                  : -1000.0;
        heights.push_back(std::llround(metres * static_cast<double>(fixed_one)));
    }
    const std::vector<std::int64_t> uplift(grid.size(), 30 * fixed_one);
    const std::vector<std::int64_t> erodibility(grid.size(), fixed_one / 10);
    const std::vector<std::int64_t> runoff(grid.size(), fixed_one / 2);
    const std::vector<std::int64_t> creep(grid.size(), 400 * fixed_one);
    const std::vector<std::int64_t> weathering(grid.size(), 5 * fixed_one);
    constexpr double steepest = 0.05;
    const Landform worn{std::move(grid),
                        std::move(heights),
                        {.uplift = uplift, .erodibility = erodibility, .runoff = runoff},
                        {.creep = creep,
                         .weathering = weathering,
                         .rock =
                             [](std::uint32_t, std::int64_t) {
                                 return hog::planet::Weathering{
                                     .rate = fixed_one,
                                     .steepest = std::llround(steepest * fixed_one)};
                             }},
                        20};
    const auto& evolved = worn.grid();
    const auto ground = worn.heights();
    int soiled = 0;
    for (std::uint32_t cell = 0; cell < evolved.size(); ++cell) {
        soiled += worn.soil()[cell] > fixed_one / 10 ? 1 : 0;
        if (ground[cell] <= 0) {
            continue;
        }
        const auto around = evolved.neighbors(cell);
        for (std::size_t k = 0; k < around.size(); ++k) {
            const double rise = hog::planet::fixed_to_double(ground[cell] - ground[around[k]]);
            if (rise > (steepest * hog::planet::fixed_to_double(evolved.distance(cell, k))) + 0.1) {
                FAIL("cell " << hog::planet::fixed_to_double(ground[cell]) << " lower "
                             << hog::planet::fixed_to_double(ground[around[k]]) << " distance "
                             << hog::planet::fixed_to_double(evolved.distance(cell, k)) << " soil "
                             << hog::planet::fixed_to_double(worn.soil()[cell]));
            }
        }
    }
    CHECK(soiled > static_cast<int>(evolved.size()) / 10);
}

TEST_CASE("a landform keeps how far each cell rose while it stood above the sea",
          "[planet][landform]") {
    LevelGrid grid{4, 115'000.0};
    std::vector<std::int64_t> heights;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const double z = hog::planet::cell_center(grid.cell(cell)).z;
        heights.push_back(z > 0.0 ? 2'000 * fixed_one : -1'000 * fixed_one);
    }
    const std::vector<std::int64_t> uplift(grid.size(), 30 * fixed_one);
    const std::vector<std::int64_t> erodibility(grid.size(), fixed_one / 100);
    const std::vector<std::int64_t> runoff(grid.size(), fixed_one / 2);
    constexpr int steps = 5;
    const std::vector<std::int64_t> start = heights;
    const Landform worn{std::move(grid),
                        std::move(heights),
                        {.uplift = uplift, .erodibility = erodibility, .runoff = runoff},
                        hog::planet::Hillslope{},
                        steps};
    for (std::uint32_t cell = 0; cell < worn.grid().size(); ++cell) {
        REQUIRE(worn.risen()[cell] == (start[cell] > 0 ? std::int64_t{steps} * 30 * fixed_one : 0));
    }
}

TEST_CASE("a landform's steepness is its steepest slope to a neighbouring cell",
          "[planet][landform]") {
    LevelGrid grid{4, 115'000.0};
    const std::uint32_t raised = 1'000;
    std::vector<std::int64_t> heights(grid.size(), 50 * fixed_one);
    heights[raised] = 150 * fixed_one;
    const std::vector<std::uint32_t> around{grid.neighbors(raised).begin(),
                                            grid.neighbors(raised).end()};
    const std::int64_t nearest = std::ranges::min(
        std::views::iota(std::size_t{0}, around.size()) |
        std::views::transform([&](std::size_t k) { return grid.distance(raised, k); }));
    const std::vector<std::int64_t> zeros(grid.size(), 0);
    const Landform landform{std::move(grid), heights, zeros, zeros, zeros, zeros, zeros, 0};
    const auto steepness = landform.steepness();
    CHECK(hog::planet::fixed_to_double(steepness[raised]) ==
          Catch::Approx(100.0 / hog::planet::fixed_to_double(nearest)).epsilon(1e-3));
    for (std::uint32_t cell = 0; cell < landform.grid().size(); ++cell) {
        if (cell != raised && !std::ranges::contains(around, cell)) {
            REQUIRE(steepness[cell] == 0);
        } else {
            REQUIRE(steepness[cell] > 0);
        }
    }
}

TEST_CASE("glaciers cut the worn landform last, scouring its soil, and the sea fills their troughs",
          "[planet][landform]") {
    // Cells some 250 m across, under an ice cap on mountains at the north pole.
    const LevelGrid grid{6, 10'000.0};
    std::vector<std::int64_t> heights;
    std::vector<std::int64_t> balance;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        const hog::DVec3 at = hog::planet::cell_center(grid.cell(cell));
        const double metres =
            at.z > 0.0 ? 20.0 + (2'000.0 * at.z) + (250.0 * at.z * std::sin(7.0 * at.x)) : -60.0;
        heights.push_back(std::llround(metres * static_cast<double>(fixed_one)));
        balance.push_back(std::llround((metres - 300.0) * 0.005 * static_cast<double>(fixed_one)));
    }
    const std::vector<std::int64_t> uplift(grid.size(), 0);
    const std::vector<std::int64_t> erodibility(grid.size(), fixed_one / 20);
    const std::vector<std::int64_t> runoff(grid.size(), fixed_one / 2);
    const std::vector<std::int64_t> creep(grid.size(), 400 * fixed_one);
    const std::vector<std::int64_t> weathering(grid.size(), 5 * fixed_one);
    const auto worn = [&](const hog::planet::Glaciation& glaciation) {
        return Landform{grid,
                        heights,
                        {.uplift = uplift, .erodibility = erodibility, .runoff = runoff},
                        {.creep = creep,
                         .weathering = weathering,
                         .rock =
                             [](std::uint32_t, std::int64_t) {
                                 return hog::planet::Weathering{.rate = fixed_one,
                                                                .steepest = fixed_one};
                             }},
                        3,
                        {},
                        glaciation};
    };
    const Landform bare = worn({});
    const Landform cut =
        worn({.balance = [&](std::uint32_t cell, std::int64_t) { return balance[cell]; }});
    CHECK(std::ranges::all_of(bare.carved(), [](std::int64_t depth) { return depth == 0; }));
    int drowned = 0;
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        REQUIRE(cut.heights()[cell] == heights[cell] + cut.change()[cell] - cut.carved()[cell]);
        const std::int64_t depth = bare.heights()[cell] - cut.heights()[cell];
        REQUIRE(depth >= 0);
        // Deep troughs take in the change of the ground they were cut from.
        if (depth > 20 * fixed_one) {
            REQUIRE(cut.change()[cell] == 0);
        }
        REQUIRE(cut.soil()[cell] == std::max<std::int64_t>(bare.soil()[cell] - depth, 0));
        drowned += bare.heights()[cell] > 0 && cut.drainage().surface[cell] == 0 ? 1 : 0;
    }
    CHECK(drowned > 0);
}

TEST_CASE("meanders find again the point they shifted", "[planet][landform]") {
    constexpr double radius = 115'000.0;
    // Fine enough for the bends to keep to 500 m cells, and coarse.
    for (const int level : {6, 10}) {
        const LevelGrid grid{level, radius};
        const hog::planet::Meander meander{7, grid, radius};
        // The shift steps by a part in fixed_one of how far it reaches, most of a cell, as near as
        // it can tell points apart.
        const double spacing = std::max(hog::planet::fixed_to_double(grid.distance(0, 0)), 500.0);
        const double step = spacing / static_cast<double>(fixed_one);
        std::mt19937_64 random{11};
        std::normal_distribution<double> normal;
        for (int at = 0; at < 2'000; ++at) {
            const hog::DVec3 point =
                glm::normalize(hog::DVec3{normal(random), normal(random), normal(random)});
            const hog::DVec3 shifted = meander(point);
            const hog::DVec3 found = meander.unshifted(shifted);
            REQUIRE(glm::length(meander(found) - shifted) * radius < 4.0 * step);
            REQUIRE(glm::length(found - point) * radius < 8.0 * step);
        }
    }
}
