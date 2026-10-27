#include <hog/planet/glaciers.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using hog::planet::fixed_one;
using hog::planet::glaciate;
using hog::planet::LevelGrid;

namespace {

// Cells some 250 m across.
const LevelGrid& grid() {
    static const LevelGrid grid{6, 10'000.0};
    return grid;
}

hog::DVec3 center(std::uint32_t cell) {
    return hog::planet::cell_center(grid().cell(cell));
}

std::int64_t fixed(double metres) {
    return std::llround(metres * static_cast<double>(fixed_one));
}

// Mountains 2 km high at the north pole, falling through valleys to a coast at the equator, and a
// shallow sea south of it.
std::vector<std::int64_t> land() {
    std::vector<std::int64_t> heights;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        const hog::DVec3 at = center(cell);
        const double around = std::atan2(at.y, at.x);
        heights.push_back(
            fixed(at.z > 0.0 ? 20.0 + (2'000.0 * at.z) + (250.0 * at.z * std::sin(7.0 * around))
                             : -60.0));
    }
    return heights;
}

// Snow piling up above `snowline` metres and melting below, more the further from it.
std::vector<std::int64_t> carve(double snowline, double per_metre = 0.005) {
    return glaciate(grid(), land(), {.balance = [=](std::uint32_t, std::int64_t height) {
                        return fixed((hog::planet::fixed_to_double(height) - snowline) * per_metre);
                    }});
}

} // namespace

TEST_CASE("glaciers cut nothing where no snow outlasts the summer", "[planet][glaciers]") {
    const std::vector<std::int64_t> carved = carve(5'000.0);
    CHECK(std::ranges::all_of(carved, [](std::int64_t cut) { return cut == 0; }));
}

TEST_CASE("ice reaching the sea cuts its trough down below it, and floats off",
          "[planet][glaciers]") {
    const std::vector<std::int64_t> heights = land();
    const std::vector<std::int64_t> carved = carve(300.0);
    int drowned = 0;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        drowned += heights[cell] > 0 && heights[cell] - carved[cell] < -50 * fixed_one ? 1 : 0;
        // Out at sea, past the trough's mouth.
        if (center(cell).z < -0.2) {
            CHECK(carved[cell] == 0);
        }
    }
    CHECK(drowned > 0);
    CHECK(std::ranges::max(carved) > 300 * fixed_one);
}

TEST_CASE("high ground with a snowline cuts troughs only in the valleys the ice runs down",
          "[planet][glaciers]") {
    const std::vector<std::int64_t> carved = carve(1'200.0);
    const auto cut = std::ranges::count_if(carved, [](std::int64_t depth) { return depth > 0; });
    CHECK(cut > 0);
    CHECK(cut * 4 < static_cast<std::ptrdiff_t>(carved.size()));
}

TEST_CASE("the more ice, the deeper and wider its troughs", "[planet][glaciers]") {
    const std::vector<std::int64_t> some = carve(800.0, 0.002);
    const std::vector<std::int64_t> more = carve(800.0, 0.008);
    const auto cut = [](const std::vector<std::int64_t>& carved) {
        return std::ranges::count_if(carved, [](std::int64_t depth) { return depth > 0; });
    };
    CHECK(std::ranges::max(more) > std::ranges::max(some));
    CHECK(cut(more) > cut(some));
}

TEST_CASE("a trough's floor is broad, cut nearly as deep beside its middle as in it",
          "[planet][glaciers]") {
    const std::vector<std::int64_t> heights = land();
    const std::vector<std::int64_t> carved = carve(300.0);
    // The deepest cut inland, clear of the coast.
    std::uint32_t deepest = 0;
    for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
        if (center(cell).z > 0.3 && carved[cell] > carved[deepest]) {
            deepest = cell;
        }
    }
    REQUIRE(carved[deepest] > 200 * fixed_one);
    for (const std::uint32_t beside : grid().neighbors(deepest)) {
        if (heights[beside] >= heights[deepest]) {
            CHECK(carved[beside] * 2 > carved[deepest]);
        }
    }
}

TEST_CASE("glaciers cut the same troughs every time", "[planet][glaciers]") {
    CHECK(carve(600.0) == carve(600.0));
}

TEST_CASE("ice spreading over lowland barely cuts it, falling down mountains it cuts deep",
          "[planet][glaciers]") {
    // Snow everywhere, on land rising gently and on land rising steeply.
    const auto cut_into = [](double rising) {
        std::vector<std::int64_t> heights;
        for (std::uint32_t cell = 0; cell < grid().size(); ++cell) {
            const double z = center(cell).z;
            heights.push_back(fixed(z > 0.0 ? 1.0 + (rising * z) : -60.0));
        }
        return std::ranges::max(glaciate(
            grid(), heights, {.balance = [](std::uint32_t, std::int64_t) { return fixed_one; }}));
    };
    CHECK(cut_into(60.0) * 10 < cut_into(2'000.0));
}
