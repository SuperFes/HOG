#include <hog/core/hash.hpp>
#include <hog/grid/key.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

using hog::grid::code;
using hog::grid::from_code;
using hog::grid::Key;
using hog::grid::max_depth;
using hog::grid::morton;

namespace {

Key drawn(std::uint64_t n, std::uint8_t depth) {
    const auto draw = [&](std::uint64_t seed) {
        return static_cast<std::uint32_t>(hog::hash_values(seed, n) &
                                          ((std::uint64_t{1} << depth) - 1U));
    };
    return {.depth = depth, .x = draw(1), .y = draw(2), .z = draw(3)};
}

Key drawn(std::uint64_t n) {
    return drawn(n, static_cast<std::uint8_t>(hog::hash_values(4, n) % (max_depth + 1U)));
}

} // namespace

TEST_CASE("keys come back from their codes", "[grid]") {
    for (std::uint64_t n = 0; n < 2000; ++n) {
        const Key key = drawn(n);
        REQUIRE(key.is_valid());
        CHECK(from_code(code(key)) == key);
    }
    const std::uint32_t last = (std::uint32_t{1} << max_depth) - 1U;
    for (const Key key : {Key{}, Key{.depth = max_depth},
                          Key{.depth = max_depth, .x = last, .y = last, .z = last}}) {
        CHECK(from_code(code(key)) == key);
    }
    CHECK(code(Key{}) == 1);
}

TEST_CASE("codes of different nodes differ", "[grid]") {
    CHECK(code(Key{}) != code(Key{.depth = 1}));
    CHECK(code(Key{.depth = 1, .x = 1}) != code(Key{.depth = 2, .x = 1}));
    CHECK(code(Key{.depth = 3, .x = 1}) != code(Key{.depth = 3, .y = 1}));
}

TEST_CASE("a node's eight children follow one another in Z order", "[grid]") {
    for (std::uint64_t n = 0; n < 500; ++n) {
        const Key key = drawn(n, static_cast<std::uint8_t>(hog::hash_values(5, n) % max_depth));
        for (std::size_t octant = 0; octant < 8; ++octant) {
            const Key child = key.child(octant);
            CHECK(morton(child) == (morton(key) * 8) + octant);
            CHECK(code(child) >> 3U == code(key));
        }
    }
}

TEST_CASE("parents, children and ancestors agree", "[grid]") {
    for (std::uint64_t n = 0; n < 500; ++n) {
        const Key key = drawn(n, static_cast<std::uint8_t>(1 + (hog::hash_values(6, n) % 20)));
        for (std::size_t octant = 0; octant < 8; ++octant) {
            CHECK(key.child(octant).octant() == octant);
            CHECK(key.child(octant).parent() == key);
        }
        CHECK(key.ancestor(key.depth) == key);
        CHECK(key.ancestor(static_cast<std::uint8_t>(key.depth - 1U)) == key.parent());
        CHECK(key.ancestor(0) == Key{});
        CHECK(key.parent().contains(key));
        CHECK(Key{}.contains(key));
        CHECK_FALSE(key.contains(key.parent()));
    }
    const Key key{.depth = 4, .x = 5, .y = 6, .z = 7};
    CHECK_FALSE(key.contains(Key{.depth = 4, .x = 5, .y = 6, .z = 8}));
    CHECK_FALSE(key.contains(Key{.depth = 5, .x = 12, .y = 12, .z = 14}));
    CHECK(key.contains(Key{.depth = 5, .x = 11, .y = 12, .z = 14}));
}

TEST_CASE("neighbours are a step along the grid and stop at the root's sides", "[grid]") {
    CHECK_FALSE(Key{}.beside(1, 0, 0));
    const Key corner{.depth = 3};
    CHECK_FALSE(corner.beside(-1, 0, 0));
    CHECK_FALSE(corner.beside(0, -1, 0));
    CHECK_FALSE(corner.beside(0, 0, -1));
    CHECK(corner.beside(1, 1, 1) == Key{.depth = 3, .x = 1, .y = 1, .z = 1});
    const Key far{.depth = 3, .x = 7, .y = 7, .z = 7};
    CHECK_FALSE(far.beside(1, 0, 0));
    CHECK_FALSE(far.beside(0, 0, 1));
    CHECK(far.beside(-7, 0, -7) == Key{.depth = 3, .x = 0, .y = 7, .z = 0});
    const Key middle{.depth = 3, .x = 3, .y = 4, .z = 5};
    CHECK(middle.beside(1, 0, 0) == Key{.depth = 3, .x = 4, .y = 4, .z = 5});
    CHECK(middle.beside(0, -1, 0) == Key{.depth = 3, .x = 3, .y = 3, .z = 5});
    CHECK(middle.beside(0, 0, 0) == middle);
}
