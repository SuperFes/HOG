#include <hog/core/hash.hpp>
#include <hog/grid/grid.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using hog::DVec3;
using hog::grid::Grid;
using hog::grid::Key;

namespace {

// Working world: 28.8 km radius and 3 km of sky, a third of a metre at the finest.
Grid working() {
    return {31'800.0, 1.0 / 3.0};
}

double unit(std::uint64_t seed, std::uint64_t n) {
    return static_cast<double>(hog::hash_values(seed, n) >> 11U) * 0x1p-53;
}

} // namespace

TEST_CASE("the root is the smallest that halves to the finest size and holds the planet",
          "[grid]") {
    CHECK(working().depth() == 18);
    CHECK(working().size(18) == 1.0 / 3.0);
    CHECK(working().size(0) >= 2 * 31'800.0);
    CHECK(working().size(1) < 2 * 31'800.0);
    CHECK(working().size(12) == working().size(18) * 64);
    CHECK(Grid{0.5, 1.0}.depth() == 0);
    CHECK(Grid{0.6, 1.0}.depth() == 1);
}

TEST_CASE("every point lies in the node holding it, at every depth", "[grid]") {
    const double half = working().size(0) / 2;
    for (std::uint64_t n = 0; n < 500; ++n) {
        const DVec3 point = (DVec3{unit(1, n), unit(2, n), unit(3, n)} * 2.0 - 1.0) * half;
        const Key finest = working().key_at(point, working().depth()).value();
        REQUIRE(finest.is_valid());
        const DVec3 low = working().low(finest);
        const double size = working().size(finest.depth);
        CHECK(glm::all(glm::greaterThanEqual(point, low - 1e-9)));
        CHECK(glm::all(glm::lessThan(point, low + size + 1e-9)));
        for (std::uint8_t depth = 0; depth < working().depth(); ++depth) {
            CHECK(working().key_at(point, depth) == finest.ancestor(depth));
        }
    }
}

TEST_CASE("a node's centre is the key of that point", "[grid]") {
    for (std::uint64_t n = 0; n < 200; ++n) {
        const auto depth = static_cast<std::uint8_t>(hog::hash_values(4, n) % 19);
        const std::uint32_t mask = (std::uint32_t{1} << depth) - 1U;
        const Key key{.depth = depth,
                      .x = static_cast<std::uint32_t>(hog::hash_values(5, n)) & mask,
                      .y = static_cast<std::uint32_t>(hog::hash_values(6, n)) & mask,
                      .z = static_cast<std::uint32_t>(hog::hash_values(7, n)) & mask};
        CHECK(working().key_at(working().centre(key), depth) == key);
    }
}

TEST_CASE("the planet's centre is the corner the middle eight share", "[grid]") {
    CHECK(working().key_at(DVec3{0.0}, 0) == Key{});
    for (std::uint8_t depth = 1; depth <= working().depth(); ++depth) {
        const std::uint32_t middle = std::uint32_t{1} << (depth - 1U);
        CHECK(working().key_at(DVec3{0.0}, depth) ==
              Key{.depth = depth, .x = middle, .y = middle, .z = middle});
    }
}

TEST_CASE("points outside the root have no key", "[grid]") {
    const double half = working().size(0) / 2;
    CHECK_FALSE(working().key_at(DVec3{half, 0.0, 0.0}, 5));
    CHECK_FALSE(working().key_at(DVec3{0.0, -half - 1.0, 0.0}, 5));
    CHECK_FALSE(working().key_at(DVec3{0.0, 0.0, 1e9}, 0));
    CHECK(working().key_at(DVec3{-half, -half, -half}, 5) == Key{.depth = 5});
}

TEST_CASE("pages are cut every six levels up from the finest", "[grid]") {
    const auto page_depth = [](const Grid& grid, std::uint8_t depth) {
        return grid.page_of(Key{.depth = depth}).depth;
    };
    CHECK(page_depth(working(), 0) == 0);
    CHECK(page_depth(working(), 1) == 0);
    CHECK(page_depth(working(), 6) == 0);
    CHECK(page_depth(working(), 7) == 6);
    CHECK(page_depth(working(), 12) == 6);
    CHECK(page_depth(working(), 13) == 12);
    CHECK(page_depth(working(), 18) == 12);

    // Twenty deep: the finest pages stay full, the top one holds two levels.
    const Grid deeper{400'000.0, 1.0};
    REQUIRE(deeper.depth() == 20);
    CHECK(page_depth(deeper, 1) == 0);
    CHECK(page_depth(deeper, 2) == 0);
    CHECK(page_depth(deeper, 3) == 2);
    CHECK(page_depth(deeper, 8) == 2);
    CHECK(page_depth(deeper, 9) == 8);
    CHECK(page_depth(deeper, 20) == 14);

    // Each page holds the levels down to the next cut.
    CHECK(working().levels_under(0) == 6);
    CHECK(working().levels_under(12) == 6);
    CHECK(deeper.levels_under(0) == 2);
    CHECK(deeper.levels_under(2) == 6);
    CHECK(deeper.levels_under(14) == 6);
}

TEST_CASE("a page holds its nodes, and a step past its side is in the next page", "[grid]") {
    for (std::uint64_t n = 0; n < 500; ++n) {
        const auto depth = static_cast<std::uint8_t>(hog::hash_values(8, n) % 19);
        const std::uint32_t mask = (std::uint32_t{1} << depth) - 1U;
        const Key key{.depth = depth,
                      .x = static_cast<std::uint32_t>(hog::hash_values(9, n)) & mask,
                      .y = static_cast<std::uint32_t>(hog::hash_values(10, n)) & mask,
                      .z = static_cast<std::uint32_t>(hog::hash_values(11, n)) & mask};
        const Key page = working().page_of(key);
        CHECK(page.contains(key));
        CHECK(key.depth - page.depth <= hog::grid::page_levels);
    }
    const Key edge{.depth = 18, .x = 63, .y = 70, .z = 200};
    const Key inside = edge.beside(-1, 0, 0).value();
    const Key across = edge.beside(1, 0, 0).value();
    CHECK(working().page_of(inside) == working().page_of(edge));
    CHECK(working().page_of(edge) == Key{.depth = 12, .x = 0, .y = 1, .z = 3});
    CHECK(working().page_of(across) == Key{.depth = 12, .x = 1, .y = 1, .z = 3});
}

TEST_CASE("the distance to a node is to its nearest point, none inside it", "[grid]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    const hog::grid::Key key{.depth = 3, .x = 2, .y = 5, .z = 7};
    const hog::DVec3 low = grid.low(key);
    const double size = grid.size(key.depth);
    CHECK(grid.distance(key, grid.centre(key)) == 0.0);
    CHECK(grid.distance(key, low) == 0.0);
    CHECK(grid.distance(key, low - hog::DVec3{3.0, 0.0, 0.0}) == 3.0);
    CHECK(grid.distance(key, low + hog::DVec3{size + 3.0, size + 4.0, 1.0}) == 5.0);
}
