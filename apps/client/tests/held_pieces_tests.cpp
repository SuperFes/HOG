#include <hog/client/held_pieces.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/piece.hpp>
#include <hog/grid/tree.hpp>

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

std::vector<std::byte> coded(Key root, std::optional<Tree<Material>> material,
                             std::optional<Tree<Water>> water = std::nullopt,
                             std::optional<Tree<Shape>> shape = std::nullopt) {
    hog::grid::Piece piece;
    piece.root = root;
    piece.generator = 1;
    piece.shape = std::move(shape);
    piece.material = std::move(material);
    piece.water = std::move(water);
    return piece.encode();
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

} // namespace

TEST_CASE("held pieces answer at a node from the piece reaching deepest there",
          "[client][held_pieces]") {
    const hog::grid::Grid grid = test_grid();
    HeldPieces held{grid};
    REQUIRE(held.take(coded(Key{}, stripes(5, 100))).has_value());
    const Key piece{.depth = 4, .x = 3, .y = 4, .z = 5};
    REQUIRE(held.take(coded(piece, stripes(3, 200))).has_value());
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
    REQUIRE(held.take(coded(shallow, stripes(3, 300))).has_value());
    const Key under{.depth = 3, .x = 1, .y = 2, .z = 2};
    REQUIRE(held.take(coded(under, Tree<Material>{1, 400, true})).has_value());
    const Key deep{.depth = 11, .x = under.x << 8U, .y = under.y << 8U, .z = under.z << 8U};
    CHECK(held.at<Material>(deep).value().root == shallow);
    CHECK(held.at<Material>(deep).value().found.depth == 4);
    CHECK(held.at<Material>(under).value().root == under);

    // Deeper, a piece is held in place of what was.
    REQUIRE(held.take(coded(piece, stripes(7, 500))).has_value());
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
    REQUIRE(held.take(coded(Key{}, std::nullopt, std::nullopt, ground(4))).has_value());
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
            REQUIRE(held.take(coded(child, std::nullopt, std::nullopt, ground(2))).has_value());
        }
    }
    CHECK(held.covers(node, 6));
    CHECK_FALSE(held.covers(node, 7));
}

TEST_CASE("a snapshot of held pieces holds them as they were, and finds those round a node",
          "[client][held_pieces]") {
    const hog::grid::Grid grid = test_grid();
    HeldPieces held{grid};
    REQUIRE(held.take(coded(Key{}, stripes(5, 100))).has_value());
    const Key node{.depth = 5, .x = 3, .y = 4, .z = 5};
    const Key over = node.ancestor(3);
    const Key inside = node.child(5).child(2);
    const Key touching{.depth = 7, .x = (4U * 4U), .y = 4U * 4U, .z = 5U * 4U};
    const Key beside_far{.depth = 7, .x = (4U * 4U) + 2, .y = 4U * 4U, .z = 5U * 4U};
    const Key away{.depth = 5, .x = 9, .y = 9, .z = 9};
    for (const Key root : {over, inside, touching, beside_far, away}) {
        REQUIRE(held.take(coded(root, stripes(2, 200))).has_value());
    }
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
    REQUIRE(held.take(coded(inside, stripes(2, 300))).has_value());
    CHECK(snapshot.piece(inside) != held.piece(inside));
    CHECK(snapshot.piece(inside) != nullptr);
}

TEST_CASE("a piece sent with only the trees that changed keeps the rest", "[client][held_pieces]") {
    HeldPieces held{test_grid()};
    const auto water = [](std::uint32_t body) {
        return Tree<Water>::build(5, [&](int, int, int) { return body; });
    };
    REQUIRE(held.take(coded(Key{}, stripes(5, 1), water(1))).has_value());
    const std::size_t before = held.bytes();
    REQUIRE(held.take(coded(Key{}, std::nullopt, water(2))).has_value());
    const hog::grid::Piece* piece = held.piece(Key{});
    REQUIRE(piece != nullptr);
    REQUIRE(piece->material.has_value());
    CHECK(piece->material.value().same_as(stripes(5, 1)));
    CHECK(held.at<Water>(Key{.depth = 5}).value().found.value == 2);
    CHECK(held.bytes() == before);
}

TEST_CASE("held pieces refuse what isn't a piece of the grid", "[client][held_pieces]") {
    HeldPieces held{test_grid()};
    CHECK_FALSE(held.take(std::vector<std::byte>{std::byte{1}, std::byte{2}}).has_value());
    CHECK_FALSE(held.take(coded(Key{.depth = 12}, stripes(0, 1))).has_value());
    CHECK_FALSE(held.take(coded(Key{.depth = 8}, stripes(4, 1))).has_value());
    CHECK(held.pieces() == 0);
}
