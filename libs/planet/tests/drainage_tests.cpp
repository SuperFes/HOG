#include <hog/planet/drainage.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

using hog::planet::Drainage;
using hog::planet::fixed_one;
using hog::planet::LevelGrid;

namespace {

const LevelGrid& grid() {
    static const LevelGrid grid{4, 115'000.0};
    return grid;
}

// Fixed-point metres from a function of the cell's centre.
template <typename F>
std::vector<std::int64_t> field(F metres) {
    std::vector<std::int64_t> heights;
    for (std::uint32_t index = 0; index < grid().size(); ++index) {
        const hog::DVec3 at = hog::planet::cell_center(grid().cell(index));
        heights.push_back(std::llround(metres(at) * static_cast<double>(fixed_one)));
    }
    return heights;
}

// Land rising towards the north pole, sea south of the equator, with the ground roughened so
// water has to find its way.
std::vector<std::int64_t> slope() {
    return field([](hog::DVec3 at) {
        return (1000.0 * at.z) + (40.0 * std::sin(17.0 * at.x) * std::cos(13.0 * at.y));
    });
}

// The same slope with a bowl dug around a point in the northern land.
constexpr hog::DVec3 bowl_center{0.6, 0.0, 0.8};

std::vector<std::int64_t> bowl() {
    return field([](hog::DVec3 at) {
        const double away = glm::length(at - bowl_center);
        return (1000.0 * at.z) - (600.0 * std::max(0.0, 1.0 - (away / 0.25)));
    });
}

void require_drains_to_outlets(const Drainage& drainage) {
    const std::uint32_t count = grid().size();
    REQUIRE(drainage.order.size() == count);
    std::vector<std::uint32_t> position(count);
    for (std::uint32_t at = 0; at < count; ++at) {
        position[drainage.order[at]] = at;
    }
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        const std::uint32_t receiver = drainage.receiver[cell];
        if (receiver == cell) {
            continue;
        }
        REQUIRE(std::ranges::count(grid().neighbors(cell), receiver) == 1);
        REQUIRE(drainage.surface[receiver] < drainage.surface[cell]);
        REQUIRE(position[receiver] < position[cell]);
    }
}

} // namespace

TEST_CASE("every cell drains, downhill, to an outlet", "[planet][drainage]") {
    const auto heights = slope();
    const Drainage drainage = hog::planet::drain(grid(), heights);
    require_drains_to_outlets(drainage);
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        const bool sea = heights[cell] <= 0;
        REQUIRE((drainage.receiver[cell] == cell) == sea);
        REQUIRE(drainage.surface[cell] >= std::max<std::int64_t>(heights[cell], 0));
    }
}

TEST_CASE("a bowl fills to its rim and drains over it", "[planet][drainage]") {
    const auto heights = bowl();
    const Drainage drainage = hog::planet::drain(grid(), heights);
    require_drains_to_outlets(drainage);
    const std::uint32_t bottom = grid().index(hog::planet::cell_at(bowl_center, grid().level()));
    const std::int64_t depth = drainage.surface[bottom] - heights[bottom];
    CHECK(depth > 100 * fixed_one);
    // Lake levels rise by a hair per cell towards the outlet, never more.
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        if (drainage.surface[cell] > heights[cell] && heights[cell] > 0) {
            REQUIRE(drainage.surface[cell] - drainage.surface[bottom] <
                    static_cast<std::int64_t>(grid().size()));
        }
    }
}

TEST_CASE("ground already sloping to the sea is left as it is", "[planet][drainage]") {
    const auto heights = field([](hog::DVec3 at) { return 1000.0 * at.z; });
    const Drainage drainage = hog::planet::drain(grid(), heights);
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        REQUIRE(drainage.surface[cell] == std::max<std::int64_t>(heights[cell], 0));
    }
}

TEST_CASE("a world without sea drains through its lowest cell", "[planet][drainage]") {
    auto heights = slope();
    const std::int64_t lowest = std::ranges::min(heights);
    for (std::int64_t& height : heights) {
        height += fixed_one - lowest;
    }
    const Drainage drainage = hog::planet::drain(grid(), heights);
    require_drains_to_outlets(drainage);
    const auto outlets = std::ranges::count_if(
        drainage.order, [&](std::uint32_t cell) { return drainage.receiver[cell] == cell; });
    CHECK(outlets == 1);
    CHECK(heights[drainage.order.front()] == fixed_one);
}

TEST_CASE("what collects downstream adds up", "[planet][drainage]") {
    const auto heights = slope();
    const Drainage drainage = hog::planet::drain(grid(), heights);
    std::vector<std::int64_t> rain(grid().size());
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        rain[cell] = heights[cell] > 0 ? grid().area(cell) : 0;
    }
    const auto total = hog::planet::accumulate(drainage, rain);
    std::int64_t at_outlets = 0;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        REQUIRE(total[cell] >= rain[cell]);
        if (drainage.receiver[cell] == cell) {
            at_outlets += total[cell];
        } else {
            REQUIRE(total[drainage.receiver[cell]] >= total[cell]);
        }
    }
    CHECK(at_outlets == std::accumulate(rain.begin(), rain.end(), std::int64_t{0}));
}
