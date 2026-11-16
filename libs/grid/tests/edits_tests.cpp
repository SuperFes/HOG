#include <hog/grid/edits.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using hog::grid::Edit;
using hog::grid::EditLog;
using hog::grid::Held;
using hog::grid::Key;
using hog::grid::Material;
using hog::grid::Page;
using hog::grid::Shape;
using hog::grid::Tree;

namespace {

constexpr hog::grid::Surface solid = hog::grid::Surface::solid();
constexpr hog::grid::Surface open = hog::grid::Surface::open();

// Rooted twelve deep, solid and open in a checkerboard of blocks four across, all of one
// material.
Page baked() {
    return Page{.root = Key{.depth = 12, .x = 1, .y = 2, .z = 3},
                .generator = 7,
                .shape = Held<Shape>{.version = 1,
                                     .tree = Tree<Shape>::build(
                                         6,
                                         [](int x, int y, int z) {
                                             return ((x / 4) + (y / 4) + (z / 4)) % 2 == 0 ? solid
                                                                                           : open;
                                         })},
                .material = Held<Material>{.version = 1, .tree = Tree<Material>{6, 3, false}},
                .water = std::nullopt};
}

// The finest node at (x, y, z) within the baked page.
Key in_page(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return {.depth = 18, .x = 64 + x, .y = 128 + y, .z = 192 + z};
}

EditLog dug() {
    EditLog log;
    // Clears four finest a side at the page's low corner, then fills one back.
    log.edits.push_back(Edit{.id = 1,
                             .author = 11,
                             .time = 1'763'000'000'000,
                             .generator = 7,
                             .left = {{.key = Key{.depth = 16, .x = 16, .y = 32, .z = 48},
                                       .shape = open,
                                       .material = std::nullopt,
                                       .water = std::nullopt}}});
    log.edits.push_back(Edit{.id = 2,
                             .author = 12,
                             .time = 1'763'000'060'000,
                             .generator = 7,
                             .left = {{.key = in_page(1, 1, 1),
                                       .shape = solid,
                                       .material = Material::Value{9},
                                       .water = std::nullopt}}});
    return log;
}

} // namespace

TEST_CASE("an edit over a baked page reads through to it", "[grid]") {
    const Page page = baked();
    const EditLog none;
    CHECK(none.read<Shape>(page, in_page(0, 0, 0)) == solid);
    CHECK(none.read<Shape>(page, in_page(44, 0, 0)) == open);
    CHECK(none.read<Material>(page, in_page(0, 0, 0)) == Material::Value{3});

    const EditLog log = dug();
    CHECK(log.read<Shape>(page, in_page(0, 0, 0)) == open);
    CHECK(log.read<Shape>(page, in_page(3, 3, 3)) == open);
    CHECK(log.read<Shape>(page, in_page(1, 1, 1)) == solid);
    CHECK(log.read<Shape>(page, in_page(8, 0, 0)) == solid);
    CHECK(log.read<Shape>(page, in_page(4, 0, 0)) == open);
    CHECK(log.read<Material>(page, in_page(1, 1, 1)) == Material::Value{9});
    CHECK(log.read<Material>(page, in_page(0, 0, 0)) == Material::Value{3});
}

TEST_CASE("nothing reads where neither an edit nor the page says", "[grid]") {
    const Page page = baked();
    const EditLog log = dug();
    CHECK_FALSE(log.read<hog::grid::Water>(page, in_page(0, 0, 0)));
    CHECK_FALSE(log.read<Shape>(page, Key{.depth = 18}));
    CHECK_FALSE(log.read<Shape>(page, Key{.depth = 19, .x = 208, .y = 256, .z = 384}));
    CHECK(log.read<Shape>(page, Key{.depth = 19, .x = 128, .y = 256, .z = 384}) == open);
    CHECK(log.read<Shape>(page, page.root) == page.shape.value().tree.root().value);
}

TEST_CASE("edit logs come back from their bytes", "[grid]") {
    const EditLog log = dug();
    const auto back = EditLog::decode(log.encode());
    REQUIRE(back);
    CHECK(*back == log);
    const auto empty = EditLog::decode(EditLog{}.encode());
    REQUIRE(empty);
    CHECK(empty->edits.empty());
}

TEST_CASE("broken edit log bytes are refused", "[grid]") {
    const std::vector<std::byte> bytes = dug().encode();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CHECK_FALSE(EditLog::decode(std::span{bytes}.first(length)));
    }
    std::vector<std::byte> format = bytes;
    format[0] = std::byte{99};
    CHECK_FALSE(EditLog::decode(format));

    EditLog outside;
    outside.edits.push_back(Edit{.left = {{.key = Key{.depth = 3, .x = 8},
                                           .shape = std::nullopt,
                                           .material = std::nullopt,
                                           .water = std::nullopt}}});
    CHECK_FALSE(EditLog::decode(outside.encode()));

    EditLog plain;
    plain.edits.push_back(Edit{.left = {Edit::Left{}}});
    std::vector<std::byte> kinds = plain.encode();
    REQUIRE(EditLog::decode(kinds));
    kinds.back() = std::byte{8};
    CHECK_FALSE(EditLog::decode(kinds));
}
