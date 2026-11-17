#include <hog/core/hash.hpp>
#include <hog/grid/tree.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using hog::grid::Material;
using hog::grid::Tree;
using hog::grid::Water;

namespace {

constexpr std::uint8_t levels = 4;
constexpr std::uint32_t across = 1U << levels;

// Patches of a few materials, alike in blocks of two or four so some merge and some don't.
Material::Value patchy(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t seed = 1) {
    return static_cast<Material::Value>(hog::hash_values(seed, x / 2, y / 2, z / 4) % 3);
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

TEST_CASE("material and water summarise as the most common but none", "[grid]") {
    CHECK(Material::summary({1, 2, 2, 3, 3, 3, 0, 0}) == 3);
    CHECK(Material::summary({5, 4, 5, 4, 1, 2, 6, 7}) == 4);
    CHECK(Material::summary({0, 0, 0, 0, 0, 2, 1, 2}) == 2);
    CHECK(Material::summary({0, 0, 0, 0, 0, 0, 0, 0}) == 0);
    CHECK(Water::summary({0, 0, 0, 0, 0, 9, 9, 9}) == 9);
    CHECK(Water::summary({9, 9, 9, 9, 0, 0, 0, 0}) == 9);
}

TEST_CASE("eight alike merge into one", "[grid]") {
    const auto whole = Tree<Water>::build(levels, [](int, int, int) { return Water::Value{77}; });
    CHECK(whole.nodes() == 1);
    CHECK(whole.root() == Tree<Water>::Found{.value = 77, .open = false, .depth = 0});
    CHECK(whole.at(levels, 3, 4, 5) == Tree<Water>::Found{.value = 77, .open = false, .depth = 0});

    const auto halves = Tree<Water>::build(
        levels, [](int x, int, int) { return static_cast<Water::Value>(x < 8 ? 5 : 6); });
    CHECK(halves.nodes() == 3);
    CHECK(halves.leaves() == 2);
    CHECK(halves.root().value == 5);
    CHECK(halves.at(levels, 2, 9, 9) == Tree<Water>::Found{.value = 5, .open = false, .depth = 1});
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
    check_summaries(Tree<Water>::build(levels, [](int x, int y, int z) {
        return static_cast<Water::Value>(hog::hash_values(2, x, y / 4, z) % 5);
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

    const Tree<Water> open{6, 1234, true};
    const auto open_back = Tree<Water>::decode(open.encode());
    REQUIRE(open_back);
    CHECK(open_back->root() == Tree<Water>::Found{.value = 1234, .open = true, .depth = 0});
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
    const std::size_t root = split.size() - 8 - 2 - 1;
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

TEST_CASE("growing down to the foot gives what building up does", "[grid]") {
    const auto grown = Tree<Material>::grow(
        levels,
        [](std::uint8_t depth, std::uint32_t x, std::uint32_t y,
           std::uint32_t z) -> std::optional<Tree<Material>::Leaf> {
            if (depth < levels) {
                return std::nullopt;
            }
            return Tree<Material>::Leaf{.value = patchy(x, y, z), .open = false};
        });
    CHECK(grown.same_as(patchy_tree()));
}

TEST_CASE("growing stops where a node is decided a leaf", "[grid]") {
    const auto whole =
        Tree<Water>::grow(levels, [](std::uint8_t, std::uint32_t, std::uint32_t, std::uint32_t) {
            return std::optional{Tree<Water>::Leaf{.value = 3, .open = true}};
        });
    CHECK(whole.nodes() == 1);
    CHECK(whole.root() == Tree<Water>::Found{.value = 3, .open = true, .depth = 0});

    // One corner split to the foot, the rest leaves at the top.
    const auto corner = Tree<Water>::grow(
        levels,
        [](std::uint8_t depth, std::uint32_t x, std::uint32_t y,
           std::uint32_t z) -> std::optional<Tree<Water>::Leaf> {
            if (x == 0 && y == 0 && z == 0 && depth < levels) {
                return std::nullopt;
            }
            return Tree<Water>::Leaf{.value = depth == levels ? 1U : 2U, .open = false};
        });
    // The foot's eight alike merge into the node over them.
    CHECK(corner.at(levels, 0, 0, 0) == Tree<Water>::Found{.value = 1, .open = false, .depth = 3});
    CHECK(corner.at(levels, 15, 15, 15) ==
          Tree<Water>::Found{.value = 2, .open = false, .depth = 1});
    CHECK(corner.at(1, 0, 0, 0) == Tree<Water>::Found{.value = 2, .open = false, .depth = 1});
}

TEST_CASE("a tree trimmed keeps what lies over the cut, the nodes split there opened", "[grid]") {
    const Tree<Material> tree = patchy_tree();
    CHECK(tree.trimmed(levels).same_as(tree));
    for (std::uint8_t cut = 0; cut < levels; ++cut) {
        CAPTURE(cut);
        const Tree<Material> trimmed = tree.trimmed(cut);
        CHECK(trimmed.levels() == cut);
        const std::uint32_t side = 1U << cut;
        for (std::uint32_t n = 0; n < side * side * side; ++n) {
            const std::uint32_t x = n % side;
            const std::uint32_t y = (n / side) % side;
            const std::uint32_t z = n / (side * side);
            const auto whole = tree.at(cut, x, y, z);
            const auto kept = trimmed.at(cut, x, y, z);
            CHECK(kept.value == whole.value);
            // Opened alike, eight may merge into the node over them.
            CHECK(kept.depth <= whole.depth);
            const bool split = tree.at(cut + 1, 2 * x, 2 * y, 2 * z).depth > cut;
            CHECK(kept.open == (whole.open || split));
        }
    }
}
