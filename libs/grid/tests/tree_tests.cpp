#include <hog/core/hash.hpp>
#include <hog/grid/tree.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using hog::grid::Material;
using hog::grid::Shape;
using hog::grid::Tree;
using hog::grid::Water;

namespace {

constexpr std::uint8_t levels = 4;
constexpr std::uint32_t across = 1U << levels;

// Patches of a few materials, alike in blocks of two or four so some merge and some don't.
std::uint8_t patchy(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t seed = 1) {
    return static_cast<std::uint8_t>(hog::hash_values(seed, x / 2, y / 2, z / 4) % 3);
}

Tree<Material> patchy_tree(std::uint64_t seed = 1) {
    return Tree<Material>::build(levels, [&](int x, int y, int z) {
        return patchy(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                      static_cast<std::uint32_t>(z), seed);
    });
}

template <typename Kind>
void check_summaries(const Tree<Kind>& tree) {
    for (std::uint8_t depth = 0; depth < tree.levels(); ++depth) {
        const std::uint32_t side = 1U << depth;
        for (std::uint32_t n = 0; n < side * side * side; ++n) {
            const std::uint32_t x = n % side;
            const std::uint32_t y = (n / side) % side;
            const std::uint32_t z = n / (side * side);
            const auto node = tree.at(depth, x, y, z);
            if (node.depth != depth) {
                continue;
            }
            std::array<typename Kind::Value, 8> children{};
            bool split = false;
            for (std::uint32_t k = 0; k < 8; ++k) {
                const auto child = tree.at(static_cast<std::uint8_t>(depth + 1), (2 * x) + (k & 1U),
                                           (2 * y) + ((k >> 1U) & 1U), (2 * z) + ((k >> 2U) & 1U));
                split = split || child.depth == depth + 1;
                children.at(k) = child.value;
            }
            if (split) {
                CHECK(node.value == Kind::summary(children));
            }
        }
    }
}

} // namespace

TEST_CASE("shape summarises as the mean, material and water as the most common", "[grid]") {
    CHECK(Shape::summary({0, 0, 0, 0, 0, 0, 0, 0}) == 0);
    CHECK(Shape::summary({65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535}) == 65535);
    CHECK(Shape::summary({65535, 65535, 65535, 65535, 0, 0, 0, 0}) == 32768);
    CHECK(Shape::summary({8, 0, 0, 0, 0, 0, 0, 0}) == 1);
    CHECK(Shape::summary({3, 0, 0, 0, 0, 0, 0, 0}) == 0);
    CHECK(Material::summary({1, 2, 2, 3, 3, 3, 0, 0}) == 3);
    CHECK(Material::summary({5, 4, 5, 4, 1, 2, 6, 7}) == 4);
    CHECK(Water::summary({0, 0, 0, 0, 0, 9, 9, 9}) == 0);
    CHECK(Water::summary({9, 9, 9, 9, 0, 0, 0, 0}) == 0);
}

TEST_CASE("eight alike merge into one", "[grid]") {
    const auto whole = Tree<Shape>::build(levels, [](int, int, int) { return Shape::Value{77}; });
    CHECK(whole.nodes() == 1);
    CHECK(whole.root() == Tree<Shape>::Found{.value = 77, .open = false, .depth = 0});
    CHECK(whole.at(levels, 3, 4, 5) == Tree<Shape>::Found{.value = 77, .open = false, .depth = 0});

    const auto halves = Tree<Shape>::build(
        levels, [](int x, int, int) { return static_cast<Shape::Value>(x < 8 ? 65535 : 0); });
    CHECK(halves.nodes() == 3);
    CHECK(halves.leaves() == 2);
    CHECK(halves.root().value == 32768);
    CHECK(halves.at(levels, 2, 9, 9) ==
          Tree<Shape>::Found{.value = 65535, .open = false, .depth = 1});
}

TEST_CASE("a tree reads back what it was built from", "[grid]") {
    const Tree<Material> tree = patchy_tree();
    for (std::uint32_t z = 0; z < across; ++z) {
        for (std::uint32_t y = 0; y < across; ++y) {
            for (std::uint32_t x = 0; x < across; ++x) {
                const auto found = tree.at(levels, x, y, z);
                CHECK(found.value == patchy(x, y, z));
                CHECK_FALSE(found.open);
            }
        }
    }
    CHECK(tree.leaves() < std::size_t{across} * across * across);
}

TEST_CASE("a split node's value is its children's summary", "[grid]") {
    check_summaries(patchy_tree());
    check_summaries(Tree<Shape>::build(levels, [](int x, int y, int z) {
        return static_cast<Shape::Value>(hog::hash_values(2, x, y / 4, z) % 65536);
    }));
}

TEST_CASE("the same content gives the same bytes", "[grid]") {
    CHECK(patchy_tree().encode() == patchy_tree().encode());
    CHECK(patchy_tree(1).encode() != patchy_tree(2).encode());
    const auto once = Tree<Water>::build(3, [](int, int, int) { return Water::Value{4}; });
    CHECK(once.encode() == Tree<Water>{3, 4, false}.encode());
    CHECK(once.encode() != Tree<Water>{3, 4, true}.encode());
}

TEST_CASE("trees come back from their bytes", "[grid]") {
    const Tree<Material> tree = patchy_tree();
    const auto back = Tree<Material>::decode(tree.encode());
    REQUIRE(back);
    CHECK(back->same_as(tree));
    CHECK(back->levels() == levels);
    CHECK(back->nodes() == tree.nodes());
    for (std::uint32_t z = 0; z < across; ++z) {
        for (std::uint32_t y = 0; y < across; ++y) {
            for (std::uint32_t x = 0; x < across; ++x) {
                CHECK(back->at(levels, x, y, z) == tree.at(levels, x, y, z));
            }
        }
    }

    const Tree<Shape> open{6, 1234, true};
    const auto open_back = Tree<Shape>::decode(open.encode());
    REQUIRE(open_back);
    CHECK(open_back->root() == Tree<Shape>::Found{.value = 1234, .open = true, .depth = 0});
}

TEST_CASE("broken tree bytes are refused", "[grid]") {
    const std::vector<std::byte> bytes = patchy_tree().encode();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CHECK_FALSE(Tree<Material>::decode(std::span{bytes}.first(length)));
    }
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(Tree<Material>::decode(longer));

    std::vector<std::byte> format = bytes;
    format[0] = std::byte{99};
    CHECK_FALSE(Tree<Material>::decode(format));

    // Fewer levels than the tree goes down: its patches merge at the foot, so three.
    std::vector<std::byte> shallow = bytes;
    shallow[1] = std::byte{levels - 2};
    CHECK_FALSE(Tree<Material>::decode(shallow));

    // The root is last: flags then its value, then eight children.
    const auto halves = Tree<Material>::build(
        2, [](int x, int, int) { return static_cast<Material::Value>(x < 2 ? 1 : 2); });
    const std::vector<std::byte> split = halves.encode();
    const std::size_t root = split.size() - 8 - 1 - 1;
    REQUIRE(split[root] == std::byte{1});
    std::vector<std::byte> wrong_summary = split;
    wrong_summary[root + 1] = std::byte{2};
    CHECK_FALSE(Tree<Material>::decode(wrong_summary));
    std::vector<std::byte> open_split = split;
    open_split[root] = std::byte{3};
    CHECK_FALSE(Tree<Material>::decode(open_split));
    std::vector<std::byte> flags = split;
    flags[root] = std::byte{5};
    CHECK_FALSE(Tree<Material>::decode(flags));
    std::vector<std::byte> self = split;
    self.back() = std::byte{2};
    CHECK_FALSE(Tree<Material>::decode(self));
}
