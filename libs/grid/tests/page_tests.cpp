#include <hog/grid/page.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using hog::grid::Held;
using hog::grid::Key;
using hog::grid::Material;
using hog::grid::Page;
using hog::grid::Shape;
using hog::grid::Stand;
using hog::grid::Tree;
using hog::grid::Water;
using hog::grid::Woods;

namespace {

Page full_page() {
    return Page{.root = Key{.depth = 12, .x = 5, .y = 6, .z = 7},
                .generator = 42,
                .shape = Held<Shape>{.version = 3,
                                     .tree = Tree<Shape>::build(
                                         6,
                                         [](int x, int y, int z) {
                                             return x + y > z + 40 ? hog::grid::Surface::open()
                                                                   : hog::grid::Surface::solid();
                                         })},
                .material = Held<Material>{.version = 1, .tree = Tree<Material>{6, 2, false}},
                .water = Held<Water>{.version = 9, .tree = Tree<Water>{6, 1, true}},
                .woods = Held<Woods>{.version = 2,
                                     .tree = Tree<Woods>::build(6, [](int x, int /*y*/, int z) {
                                         return z > 30 ? Stand{}
                                                       : Stand::of(x / 64.0,
                                                                   {1.0, 0.0, 0.0, 0.0, 0.0, 0.0});
                                     })}};
}

} // namespace

TEST_CASE("pages come back from their bytes", "[grid]") {
    const Page page = full_page();
    const auto back = Page::decode(page.encode());
    REQUIRE(back);
    CHECK(back->root == page.root);
    CHECK(back->generator == 42);
    REQUIRE(back->shape);
    REQUIRE(back->material);
    REQUIRE(back->water);
    CHECK(back->shape.value().version == 3);
    CHECK(back->material.value().version == 1);
    CHECK(back->water.value().version == 9);
    CHECK(back->shape.value().tree.same_as(page.shape.value().tree));
    CHECK(back->material.value().tree.same_as(page.material.value().tree));
    CHECK(back->water.value().tree.root().open);
    REQUIRE(back->woods);
    CHECK(back->woods.value().version == 2);
    CHECK(back->woods.value().tree.same_as(page.woods.value().tree));
    CHECK(back->encode() == page.encode());
}

TEST_CASE("a page may lack any of its trees", "[grid]") {
    Page page = full_page();
    page.material.reset();
    page.water.reset();
    page.woods.reset();
    const auto back = Page::decode(page.encode());
    REQUIRE(back);
    CHECK(back->shape);
    CHECK_FALSE(back->material);
    CHECK_FALSE(back->water);

    const auto empty = Page::decode(Page{}.encode());
    REQUIRE(empty);
    CHECK_FALSE(empty->shape);
}

TEST_CASE("broken page bytes are refused", "[grid]") {
    const std::vector<std::byte> bytes = full_page().encode();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CHECK_FALSE(Page::decode(std::span{bytes}.first(length)));
    }
    std::vector<std::byte> format = bytes;
    format[0] = std::byte{99};
    CHECK_FALSE(Page::decode(format));

    // Its root's depth comes second, and x, the first of its place, is 5 of 2^12.
    std::vector<std::byte> outside = bytes;
    outside[1] = std::byte{2};
    CHECK_FALSE(Page::decode(outside));

    // Which trees follow, after format, depth, x, y, z and generator.
    std::vector<std::byte> trees = bytes;
    trees[18] = std::byte{16 | 15};
    CHECK_FALSE(Page::decode(trees));

    Page deep;
    deep.shape =
        Held<Shape>{.version = 0, .tree = Tree<Shape>{7, hog::grid::Surface::solid(), false}};
    CHECK_FALSE(Page::decode(deep.encode()));
}
