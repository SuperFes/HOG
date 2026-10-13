#include <hog/planet/landform.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
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
                        30};
    }();
    return landform;
}

std::int64_t range() {
    const auto change = landform().change();
    return std::ranges::max(change) - std::ranges::min(change);
}

} // namespace

TEST_CASE("a landform's change at its own cells is the cell's", "[planet][landform]") {
    REQUIRE(range() > 100 * fixed_one);
    for (std::uint32_t cell = 0; cell < landform().grid().size(); ++cell) {
        REQUIRE(landform().change(landform().grid().cell(cell)) == landform().change()[cell]);
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

