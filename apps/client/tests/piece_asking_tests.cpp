#include "held_chunks.hpp"
#include "sloped_ground.hpp"

#include <hog/client/held_pieces.hpp>
#include <hog/client/piece_asking.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/piece.hpp>
#include <hog/grid/view.hpp>
#include <hog/grid/z_order.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ranges>
#include <vector>

using hog::client::pieces_to_ask;
using hog::client::PieceWish;
using hog::grid::Key;

namespace {

constexpr std::size_t lots = 100'000;

// The slope under `node` down to `depth`, as a server would answer: closed where it misses a
// node, its finest nodes open where the grid goes finer.
hog::grid::Piece slope_piece(const hog::grid::Grid& grid, Key node, std::uint8_t depth) {
    using Shapes = hog::grid::Tree<hog::grid::Shape>;
    const auto levels = static_cast<std::uint8_t>(depth - node.depth);
    const auto decide = [&](std::uint8_t below, std::uint32_t x, std::uint32_t y,
                            std::uint32_t z) -> std::optional<Shapes::Leaf> {
        const Key key{.depth = static_cast<std::uint8_t>(node.depth + below),
                      .x = (node.x << below) + x,
                      .y = (node.y << below) + y,
                      .z = (node.z << below) + z};
        const double side = grid.size(key.depth);
        const double above = hog::sloped::above_ground(grid.centre(key));
        if (std::abs(above) > side) {
            return Shapes::Leaf{.value = above > 0.0 ? hog::grid::Surface::open()
                                                     : hog::grid::Surface::solid(),
                                .open = false};
        }
        if (below < levels) {
            return std::nullopt;
        }
        return Shapes::Leaf{.value =
                                hog::grid::Surface::plane(hog::sloped::normal(), above / side, 1),
                            .open = key.depth < grid.depth()};
    };
    hog::grid::Piece piece;
    piece.root = node;
    piece.generator = 1;
    piece.shape = Shapes::grow(levels, decide);
    // Every kind, as the server's bands hold them.
    piece.material = hog::grid::Tree<hog::grid::Material>{levels, 1, false};
    piece.water = hog::grid::Tree<hog::grid::Water>{levels, 0, false};
    piece.woods = hog::grid::Tree<hog::grid::Woods>{levels, {}, false};
    return piece;
}

void answer(hog::client::HeldPieces& held, const std::vector<PieceWish>& wishes) {
    for (const PieceWish& wish : wishes) {
        REQUIRE(hog::testing::hold(held, slope_piece(held.grid(), wish.node, wish.depth)));
    }
}

// Along the ground, across the slope.
hog::DVec3 across() {
    return glm::normalize(glm::cross(hog::sloped::normal(), hog::DVec3{1.0, 0.0, 0.0}));
}

} // namespace

TEST_CASE("a client asks coarse first, regions apart, until all it sees is held as it wants",
          "[client][piece_asking]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    const hog::grid::View view;
    hog::client::HeldPieces held{grid};
    const hog::DVec3 eye = hog::sloped::on_ground + (hog::sloped::normal() * 2.0);

    // Holding nothing, only the root, as deep as one region goes.
    const std::vector<PieceWish> first = pieces_to_ask(held, eye, std::nullopt, view, lots);
    REQUIRE(first.size() == 1);
    CHECK(first.front().node == Key{});
    CHECK(first.front().depth == hog::client::region_levels);

    std::vector<PieceWish> wishes = first;
    int rounds = 0;
    for (; !wishes.empty() && rounds < 20; ++rounds) {
        CHECK(std::ranges::is_sorted(wishes, std::ranges::greater{}, &PieceWish::priority));
        std::vector<Key> nodes;
        for (const PieceWish& wish : wishes) {
            CAPTURE(rounds, wish.node.depth, wish.node.x, wish.node.y, wish.node.z, wish.depth);
            CHECK(wish.depth > wish.node.depth);
            CHECK(wish.depth - wish.node.depth <= hog::client::region_levels);
            CHECK(wish.depth <= grid.depth());
            // Asked only where what's over it is held already.
            CHECK((wish.node == Key{} || held.covers(wish.node, wish.node.depth)));
            nodes.push_back(wish.node);
        }
        std::ranges::sort(nodes, hog::grid::ZOrder{});
        for (std::size_t n = 1; n < nodes.size(); ++n) {
            CAPTURE(nodes[n].depth, nodes[n].x, nodes[n].y, nodes[n].z);
            CHECK_FALSE(nodes[n - 1].contains(nodes[n]));
        }
        answer(held, wishes);
        wishes = pieces_to_ask(held, eye, std::nullopt, view, lots);
    }
    CHECK(wishes.empty());
    CHECK(rounds > 2);

    // Along the ground, near and far, held as deep as it should be seen from the eye.
    for (const double metres : {0.0, 3.0, 20.0, 90.0, 400.0}) {
        const hog::DVec3 point = hog::sloped::on_ground + (across() * metres);
        const auto key = grid.key_at(point, grid.depth());
        REQUIRE(key.has_value());
        const std::uint8_t depth =
            hog::client::depth_wanted(grid, view, Key{}, glm::length(point - eye));
        CAPTURE(metres, depth);
        CHECK(held.covers(key.value().ancestor(depth), depth));
    }
}

TEST_CASE("a client asks first for what lies where it looks, and asks again as it turns",
          "[client][piece_asking]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    const hog::grid::View view;
    hog::client::HeldPieces held{grid};
    const hog::DVec3 eye = hog::sloped::on_ground + (hog::sloped::normal() * 2.0);
    for (int round = 0; round < 2; ++round) {
        answer(held, pieces_to_ask(held, eye, std::nullopt, view, lots));
    }
    const std::vector<PieceWish> plain = pieces_to_ask(held, eye, std::nullopt, view, lots);
    const std::vector<PieceWish> ahead = pieces_to_ask(held, eye, across(), view, lots);
    const std::vector<PieceWish> behind = pieces_to_ask(held, eye, -across(), view, lots);
    REQUIRE(plain.size() > 4);
    REQUIRE(ahead.size() == plain.size());
    REQUIRE(behind.size() == plain.size());
    std::size_t lowered = 0;
    for (const PieceWish& wish : ahead) {
        const auto same = std::ranges::find(plain, wish.node, &PieceWish::node);
        REQUIRE(same != plain.end());
        CHECK(wish.depth == same->depth);
        if (wish.priority < same->priority) {
            CHECK(wish.priority == same->priority * hog::client::behind_eye);
            ++lowered;
        }
    }
    CHECK(lowered > 0);
    CHECK(lowered < ahead.size());
    const auto nodes = [](const std::vector<PieceWish>& wishes) {
        return wishes | std::views::transform(&PieceWish::node);
    };
    CHECK_FALSE(std::ranges::equal(nodes(ahead), nodes(behind)));

    // No more than asked for, the nearest kept.
    const std::vector<PieceWish> few = pieces_to_ask(held, eye, std::nullopt, view, 3);
    REQUIRE(few.size() == 3);
    CHECK(std::ranges::equal(few, plain | std::views::take(3)));
}

TEST_CASE("a client asks for what's nearest first, however big what's further is",
          "[client][piece_asking]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    const hog::grid::View view;
    hog::client::HeldPieces held{grid};
    const hog::DVec3 eye = hog::sloped::on_ground + (hog::sloped::normal() * 2.0);
    for (int round = 0; round < 2; ++round) {
        answer(held, pieces_to_ask(held, eye, std::nullopt, view, lots));
    }
    const std::vector<PieceWish> wishes = pieces_to_ask(held, eye, std::nullopt, view, lots);
    REQUIRE(wishes.size() > 4);
    for (std::size_t n = 1; n < wishes.size(); ++n) {
        CAPTURE(n, wishes[n - 1].node.depth, wishes[n].node.depth);
        CHECK(grid.distance(wishes[n - 1].node, eye) <= grid.distance(wishes[n].node, eye));
    }
}

TEST_CASE("piece wishes go over the wire as asked, no more than allowed",
          "[client][piece_asking]") {
    const std::vector<PieceWish> wishes(
        hog::protocol::most_asked + 5,
        PieceWish{.node = Key{.depth = 5, .x = 1}, .depth = 8, .priority = 0.5});
    const hog::protocol::AskPieces ask = hog::client::ask_of(wishes);
    REQUIRE(ask.pieces.size() == hog::protocol::most_asked);
    CHECK(ask.pieces.front() ==
          hog::protocol::AskedPiece{.depth = 5, .x = 1, .to = 8, .priority = 0.5F});
}
