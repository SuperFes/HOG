#include <hog/grid/piece.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

using hog::grid::Key;
using hog::grid::Material;
using hog::grid::Piece;
using hog::grid::Shape;
using hog::grid::Stand;
using hog::grid::Tree;
using hog::grid::Water;
using hog::grid::Woods;

namespace {

// Deeper than a page goes, so nothing about pages bounds it.
Piece full_piece() {
    return Piece{.root = Key{.depth = 9, .x = 5, .y = 6, .z = 7},
                 .generator = 42,
                 .shape = Tree<Shape>::build(8,
                                             [](int x, int y, int z) {
                                                 return x + y > z + 160
                                                            ? hog::grid::Surface::open()
                                                            : hog::grid::Surface::solid();
                                             }),
                 .material = Tree<Material>{8, 2, false},
                 .water = Tree<Water>{3, 1, true},
                 .woods = Tree<Woods>::build(5, [](int x, int /*y*/, int z) {
                     return z > 10 ? Stand{} : Stand::of(x / 32.0, {1.0, 0.0, 0.0, 0.0, 0.0, 0.0});
                 })};
}

} // namespace

TEST_CASE("pieces come back from their bytes", "[grid][piece]") {
    const Piece piece = full_piece();
    const auto back = Piece::decode(piece.encode());
    REQUIRE(back);
    CHECK(back->root == piece.root);
    CHECK(back->generator == 42);
    REQUIRE(back->shape);
    REQUIRE(back->material);
    REQUIRE(back->water);
    REQUIRE(back->woods);
    CHECK(back->shape.value().levels() == 8);
    CHECK(back->shape.value().same_as(piece.shape.value()));
    CHECK(back->material.value().same_as(piece.material.value()));
    CHECK(back->water.value().root().open);
    CHECK(back->woods.value().same_as(piece.woods.value()));
    CHECK(back->encode() == piece.encode());
}

TEST_CASE("a piece may lack any of its trees", "[grid][piece]") {
    Piece piece = full_piece();
    piece.shape.reset();
    piece.woods.reset();
    const auto back = Piece::decode(piece.encode());
    REQUIRE(back);
    CHECK_FALSE(back->shape);
    CHECK(back->material);
    CHECK(back->water);
    CHECK_FALSE(back->woods);

    const auto empty = Piece::decode(Piece{}.encode());
    REQUIRE(empty);
    CHECK_FALSE(empty->shape);
}

TEST_CASE("bytes that aren't a piece are refused", "[grid][piece]") {
    const std::vector<std::byte> bytes = full_piece().encode();
    // Cut short anywhere.
    for (const std::size_t length : {std::size_t{0}, std::size_t{5}, bytes.size() - 1}) {
        CHECK_FALSE(Piece::decode({bytes.data(), length}));
    }
    // Bytes left over.
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(Piece::decode(longer));
    // Another format.
    std::vector<std::byte> other = bytes;
    other[0] = std::byte{2};
    CHECK_FALSE(Piece::decode(other));
    // Trees it doesn't know of.
    std::vector<std::byte> unknown = Piece{}.encode();
    unknown.back() = std::byte{16};
    CHECK_FALSE(Piece::decode(unknown));
    // Rooted outside the grid, and going deeper than the grid can.
    const auto rooted = [](Key root, std::optional<Tree<Material>> material) {
        Piece piece;
        piece.root = root;
        piece.material = std::move(material);
        return Piece::decode(piece.encode());
    };
    CHECK_FALSE(rooted(Key{.depth = 2, .x = 4}, std::nullopt));
    CHECK_FALSE(rooted(Key{.depth = hog::grid::max_depth - 2}, Tree<Material>{3, 1, false}));
    CHECK(rooted(Key{.depth = hog::grid::max_depth - 3}, Tree<Material>{3, 1, false}));
}
