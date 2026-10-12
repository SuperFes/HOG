#include <hog/planet/level_grid.hpp>
#include <hog/planet/noise.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <numbers>

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
        const auto distances = grid.distances(index);
        for (std::size_t k = 0; k < around.size(); ++k) {
            const auto back = grid.neighbors(around[k]);
            const auto at = std::ranges::find(back, index) - back.begin();
            REQUIRE(grid.distances(around[k])[static_cast<std::size_t>(at)] == distances[k]);
            const double metres = hog::planet::fixed_to_double(distances[k]);
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
