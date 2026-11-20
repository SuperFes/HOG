#include "held_chunks.hpp"

#include <hog/client/held_pieces.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/piece.hpp>
#include <hog/grid/tree.hpp>
#include <hog/protocol/messages.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

using hog::client::HeldPieces;
using hog::grid::Key;
using hog::grid::Material;
using hog::grid::Shape;
using hog::grid::Surface;
using hog::grid::Tree;
using hog::grid::Water;
using hog::testing::hold;

namespace {

// 1 m nodes 11 deep.
hog::grid::Grid test_grid() {
    return {1000.0, 1.0};
}

// Materials changing along x every two nodes, `levels` deep, from `first`.
Tree<Material> stripes(std::uint8_t levels, std::uint16_t first) {
    return Tree<Material>::build(levels, [&](int x, int /*y*/, int /*z*/) {
        return static_cast<std::uint16_t>(first + (x / 2));
    });
}

hog::grid::Piece coded(Key root, std::optional<Tree<Material>> material,
                       std::optional<Tree<Water>> water = std::nullopt,
                       std::optional<Tree<Shape>> shape = std::nullopt) {
    hog::grid::Piece piece;
    piece.root = root;
    piece.generator = 1;
    piece.shape = std::move(shape);
    piece.material = std::move(material);
    piece.water = std::move(water);
    return piece;
}

// Open space above z's middle, solid below, `levels` deep, the nodes the middle touches open
// leaves at the foot.
Tree<Shape> ground(std::uint8_t levels) {
    using Shapes = Tree<Shape>;
    const auto decide = [&](std::uint8_t depth, std::uint32_t /*x*/, std::uint32_t /*y*/,
                            std::uint32_t z) -> std::optional<Shapes::Leaf> {
        if (depth == 0) {
            return std::nullopt;
        }
        const std::uint32_t middle = 1U << (depth - 1U);
        if (z + 1 != middle && z != middle) {
            return Shapes::Leaf{.value = z >= middle ? Surface::open() : Surface::solid(),
                                .open = false};
        }
        if (depth < levels) {
            return std::nullopt;
        }
        return Shapes::Leaf{.value = Surface::plane({0.0, 0.0, 1.0}, 0.0, 1), .open = true};
    };
    return Shapes::grow(levels, decide);
}

// A piece of `shape` with every other kind too, as the server's bands hold them.
hog::grid::Piece every_kind(Key root, Tree<Shape> shape) {
    const std::uint8_t levels = shape.levels();
    hog::grid::Piece piece = coded(root, Tree<Material>{levels, 1, false},
                                   Tree<Water>{levels, 0, false}, std::move(shape));
    piece.woods = Tree<hog::grid::Woods>{levels, {}, false};
    return piece;
}

} // namespace

TEST_CASE("held pieces answer at a node from the piece reaching deepest there",
          "[client][held_pieces]") {
    const hog::grid::Grid grid = test_grid();
    HeldPieces held{grid};
    // Bands of their own, as a band's chunks never nest.
    REQUIRE(hold(held, coded(Key{}, stripes(5, 100)), 0).has_value());
    const Key piece{.depth = 4, .x = 3, .y = 4, .z = 5};
    REQUIRE(hold(held, coded(piece, stripes(3, 200)), 2).has_value());
    CHECK(held.pieces() == 2);
    CHECK(held.reach(piece) == 7);
    CHECK_FALSE(held.reach(Key{.depth = 4}).has_value());

    // Under the piece: what it holds, three levels down at most, the stripes merging in pairs.
    const Key fine{.depth = 11, .x = (3U << 7U) + 18, .y = 4U << 7U, .z = 5U << 7U};
    const auto known = held.at<Material>(fine);
    REQUIRE(known.has_value());
    CHECK(known.value().root == piece);
    CHECK(known.value().found.depth == 6);
    CHECK(known.value().found.value == 200);
    // Elsewhere, the root's.
    const Key away{.depth = 11, .x = 9, .y = 9, .z = 9};
    const auto top = held.at<Material>(away);
    REQUIRE(top.has_value());
    CHECK(top.value().root == Key{});
    CHECK(top.value().found.depth == 4);
    // At the piece's own root, the piece answers.
    CHECK(held.at<Material>(piece).value().root == piece);
    // A kind no piece holds: nothing.
    CHECK_FALSE(held.at<Water>(fine).has_value());

    // A piece rooted deeper but reaching less deep than one over it gives way to it.
    const Key shallow{.depth = 2, .x = 0, .y = 1, .z = 1};
    REQUIRE(hold(held, coded(shallow, stripes(3, 300)), 1).has_value());
    const Key under{.depth = 3, .x = 1, .y = 2, .z = 2};
    REQUIRE(hold(held, coded(under, Tree<Material>{1, 400, true}), 3).has_value());
    const Key deep{.depth = 11, .x = under.x << 8U, .y = under.y << 8U, .z = under.z << 8U};
    CHECK(held.at<Material>(deep).value().root == shallow);
    CHECK(held.at<Material>(deep).value().found.depth == 4);
    CHECK(held.at<Material>(under).value().root == under);

    // Deeper, a piece is held in place of what was.
    REQUIRE(hold(held, coded(piece, stripes(7, 500)), 4).has_value());
    CHECK(held.pieces() == 4);
    const auto deeper = held.at<Material>(fine);
    REQUIRE(deeper.has_value());
    CHECK(deeper.value().found.depth == 10);
    CHECK(deeper.value().found.value == 500 + (18 / 2));
}

TEST_CASE("held pieces cover a node where a piece reaches, a leaf closes or those under do",
          "[client][held_pieces]") {
    const hog::grid::Grid grid = test_grid();
    HeldPieces held{grid};
    CHECK_FALSE(held.covers(Key{}, 0));
    // Its shape alone isn't all of it: the rest is still to come.
    REQUIRE(hold(held, coded(Key{}, std::nullopt, std::nullopt, ground(4))).has_value());
    CHECK_FALSE(held.covers(Key{}, 4));
    REQUIRE(hold(held, every_kind(Key{}, ground(4))).has_value());
    CHECK(held.covers(Key{}, 4));
    CHECK_FALSE(held.covers(Key{}, 5));
    // Open sky and solid ground are closed leaves, held all the way down.
    const Key sky{.depth = 2, .x = 1, .y = 1, .z = 3};
    const Key rock{.depth = 2, .x = 1, .y = 1, .z = 0};
    CHECK(held.covers(sky, 11));
    CHECK(held.covers(rock, 11));
    // Where the ground runs, only as deep as the piece goes.
    const Key surface{.depth = 4, .x = 3, .y = 3, .z = 7};
    CHECK(held.covers(surface, 4));
    CHECK_FALSE(held.covers(surface, 6));
    // Under a node, each child held deeper covers it, closed ones too.
    const Key node{.depth = 3, .x = 1, .y = 1, .z = 3};
    CHECK_FALSE(held.covers(node, 6));
    for (std::size_t octant = 0; octant < 8; ++octant) {
        const Key child = node.child(octant);
        if (child.z == 7) {
            REQUIRE(hold(held, every_kind(child, ground(2))).has_value());
        }
    }
    CHECK(held.covers(node, 6));
    CHECK_FALSE(held.covers(node, 7));
}

TEST_CASE("a snapshot of held pieces holds them as they were, and finds those round a node",
          "[client][held_pieces]") {
    const hog::grid::Grid grid = test_grid();
    HeldPieces held{grid};
    REQUIRE(hold(held, coded(Key{}, stripes(5, 100)), 0).has_value());
    const Key node{.depth = 5, .x = 3, .y = 4, .z = 5};
    const Key over = node.ancestor(3);
    const Key inside = node.child(5).child(2);
    const Key touching{.depth = 7, .x = (4U * 4U), .y = 4U * 4U, .z = 5U * 4U};
    const Key beside_far{.depth = 7, .x = (4U * 4U) + 2, .y = 4U * 4U, .z = 5U * 4U};
    const Key away{.depth = 5, .x = 9, .y = 9, .z = 9};
    // Bands of their own where they nest, as a band's chunks never do.
    for (const Key root : {over, touching, beside_far, away}) {
        REQUIRE(hold(held, coded(root, stripes(2, 200)), 1).has_value());
    }
    REQUIRE(hold(held, coded(inside, stripes(2, 200)), 2).has_value());
    const hog::client::PiecesSnapshot snapshot{grid, held.entries()};
    CHECK(snapshot.piece(node.ancestor(3)) == held.piece(over));
    CHECK(snapshot.piece(node) == nullptr);
    CHECK(snapshot.at<Material>(inside.child(1)).value().root == inside);

    std::vector<Key> around;
    for (const auto& [root, piece] : snapshot.around(node)) {
        around.push_back(root);
    }
    CHECK(std::ranges::is_sorted(around, hog::grid::ZOrder{}));
    const auto has = [&](Key root) { return std::ranges::find(around, root) != around.end(); };
    CHECK(has(Key{}));
    CHECK(has(over));
    CHECK(has(inside));
    CHECK(has(touching));
    CHECK_FALSE(has(beside_far));
    CHECK_FALSE(has(away));

    // A piece taken in anew is another; the snapshot keeps the one it had.
    REQUIRE(hold(held, coded(inside, stripes(2, 300)), 2).has_value());
    CHECK(snapshot.piece(inside) != held.piece(inside));
    CHECK(snapshot.piece(inside) != nullptr);
}

TEST_CASE("a chunk of one kind leaves the other kinds held at its root", "[client][held_pieces]") {
    HeldPieces held{test_grid()};
    const auto water = [](std::uint32_t body) {
        return Tree<Water>::build(5, [&](int, int, int) { return body; });
    };
    REQUIRE(hold(held, coded(Key{}, stripes(5, 1), water(1))).has_value());
    const std::size_t before = held.bytes();
    REQUIRE(hold(held, coded(Key{}, std::nullopt, water(2))).has_value());
    const hog::grid::Piece* piece = held.piece(Key{});
    REQUIRE(piece != nullptr);
    REQUIRE(piece->material.has_value());
    CHECK(piece->material.value().same_as(stripes(5, 1)));
    CHECK(held.at<Water>(Key{.depth = 5}).value().found.value == 2);
    CHECK(held.bytes() == before);
}

TEST_CASE("a chunk from a shallower band stays under a deeper one at its root till that's let go",
          "[client][held_pieces]") {
    HeldPieces held{test_grid()};
    const Key node{.depth = 5, .x = 3, .y = 1, .z = 2};
    REQUIRE(hold(held, coded(Key{}, stripes(5, 1)), 1).has_value());
    REQUIRE(hold(held, coded(Key{}, stripes(3, 9)), 0).has_value());
    CHECK(held.at<Material>(node).value().found.depth == 4);
    CHECK(held.at<Material>(node).value().found.value == 2);
    // Let go from a band it isn't held from, it stays.
    const hog::protocol::SentChunk from_coarse{
        .kind = 1, .band = 0, .depth = 0, .x = 0, .y = 0, .z = 0, .version = 0, .tree = {}};
    REQUIRE(held.take(from_coarse).has_value());
    CHECK(held.pieces() == 1);
    hog::protocol::SentChunk from_fine = from_coarse;
    from_fine.band = 1;
    REQUIRE(held.take(from_fine).has_value());
    CHECK(held.pieces() == 0);
    CHECK(held.bytes() == 0);
    CHECK_FALSE(held.at<Material>(node).has_value());
    // The shallower one sent again is held.
    REQUIRE(hold(held, coded(Key{}, stripes(3, 9)), 0).has_value());
    CHECK(held.at<Material>(node).value().found.depth == 2);
}

TEST_CASE("a chunk taken lets go of its band's chunks of its kind over it and under it",
          "[client][held_pieces]") {
    HeldPieces held{test_grid()};
    const Key node{.depth = 2, .x = 1, .y = 1, .z = 1};
    const Key under{.depth = 3, .x = 2, .y = 3, .z = 2};
    REQUIRE(hold(held, coded(node, stripes(3, 1)), 1).has_value());
    // Of another band, it stays.
    REQUIRE(hold(held, coded(under.child(0), stripes(1, 7)), 2).has_value());
    CHECK(held.pieces() == 2);
    // The band split afresh, finer: what was held coarser of it is out of date.
    REQUIRE(hold(held, coded(under, stripes(2, 5)), 1).has_value());
    CHECK(held.piece(node) == nullptr);
    CHECK(held.piece(under) != nullptr);
    CHECK(held.piece(under.child(0)) != nullptr);
    // And coarser again: the finer one goes.
    REQUIRE(hold(held, coded(Key{.depth = 1}, stripes(4, 9)), 1).has_value());
    CHECK(held.piece(under) == nullptr);
    CHECK(held.piece(under.child(0)) != nullptr);
    CHECK(held.at<Material>(node).value().found.value >= 9);
}

TEST_CASE("held pieces refuse what isn't a chunk of the grid", "[client][held_pieces]") {
    HeldPieces held{test_grid()};
    const hog::protocol::SentChunk garbled{.kind = 1,
                                           .band = 0,
                                           .depth = 0,
                                           .x = 0,
                                           .y = 0,
                                           .z = 0,
                                           .version = 1,
                                           .tree = {std::byte{1}, std::byte{2}}};
    CHECK_FALSE(held.take(garbled).has_value());
    hog::protocol::SentChunk no_kind = hog::testing::chunks_of(coded(Key{}, stripes(1, 1))).front();
    no_kind.kind = 4;
    CHECK_FALSE(held.take(no_kind).has_value());
    CHECK_FALSE(hold(held, coded(Key{.depth = 12}, stripes(0, 1))).has_value());
    CHECK_FALSE(hold(held, coded(Key{.depth = 8}, stripes(4, 1))).has_value());
    CHECK(held.pieces() == 0);
}
