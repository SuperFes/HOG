#include "piece_world.hpp"

#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/piece.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/server/piece_sending.hpp>
#include <hog/server/piece_source.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using hog::grid::Key;
using hog::server::PieceSending;
using hog::server::PieceSource;

namespace {

constexpr hog::DVec3 here{0.2, 0.7, -0.68};
constexpr hog::DVec3 there{-0.6, 0.1, 0.79};
constexpr hog::DVec3 nowhere{0.7, -0.5, 0.5};
constexpr std::size_t lots = std::numeric_limits<std::size_t>::max();

hog::grid::Piece decoded(const PieceSending::Sent& sent) {
    auto piece = hog::grid::Piece::decode(sent.bytes);
    REQUIRE(piece.has_value());
    CHECK(piece->root == sent.node);
    return std::move(*piece);
}

} // namespace

TEST_CASE("a player is sent the pieces it asks for in its order, and none it holds again",
          "[server][piece_sending]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 9);
    const Key other = hog::testing::ground_key(generator, there, 9);
    const Key missing = hog::testing::ground_key(generator, nowhere, 9);
    hog::testing::fill_chunks(generator, store, node, 12);
    hog::testing::fill_chunks(generator, store, other, 12);
    auto source = PieceSource::open(store);
    REQUIRE(source.has_value());

    PieceSending sending{grid.depth()};
    const hog::net::ConnectionId player{1};
    sending.join(player);
    const std::vector<PieceSending::Want> wants{
        {.node = other, .depth = 12, .priority = 0.5},
        {.node = node, .depth = 12, .priority = 0.9},
        {.node = missing, .depth = 12, .priority = 0.1},
        {.node = Key{.depth = 3, .x = 8}, .depth = 6, .priority = 1.0}};
    sending.ask(player, wants);
    CHECK(sending.due(player) == 3);
    const std::vector<PieceSending::Sent> sent = sending.next(player, lots, *source);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].node == other);
    CHECK(sent[1].node == node);
    CHECK(sent[0].bytes == source->answer(other, 12)->bytes);
    CHECK(decoded(sent[1]).shape.value().same_as(source->answer(node, 12)->piece.shape.value()));
    CHECK(sending.due(player) == 0);
    CHECK(sending.waiting(player) == 1);

    // Asked again, only what it doesn't hold is due, and it still can't go.
    sending.ask(player, wants);
    CHECK(sending.due(player) == 1);
    CHECK(sending.next(player, lots, *source).empty());
    CHECK(sending.waiting(player) == 1);

    // With room for one, one goes at a time, in order.
    const hog::net::ConnectionId tight{2};
    sending.join(tight);
    sending.ask(tight, wants);
    const auto once = sending.next(tight, 1, *source);
    REQUIRE(once.size() == 1);
    CHECK(once[0].node == other);
    CHECK(sending.due(tight) == 2);
    const auto twice = sending.next(tight, 1, *source);
    REQUIRE(twice.size() == 1);
    CHECK(twice[0].node == node);
}

TEST_CASE("a player is sent a piece that changes once, with only the kinds that changed",
          "[server][piece_sending]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 9);
    const Key other = hog::testing::ground_key(generator, there, 9);
    hog::testing::fill_chunks(generator, store, node, 12);
    auto source = PieceSource::open(store);
    REQUIRE(source.has_value());

    PieceSending sending{grid.depth()};
    const hog::net::ConnectionId player{1};
    sending.join(player);
    const std::vector<PieceSending::Want> wants{{.node = node, .depth = 12, .priority = 0.5}};
    sending.ask(player, wants);
    REQUIRE(sending.next(player, lots, *source).size() == 1);

    hog::testing::put_material(store, 3, node, 7);
    REQUIRE(source->pump().has_value());
    const PieceSource::Touched touched{.node = node, .from = 10, .to = 12};
    sending.changed({&touched, 1});
    CHECK(sending.due(player) == 1);
    const auto sent = sending.next(player, lots, *source);
    REQUIRE(sent.size() == 1);
    const hog::grid::Piece piece = decoded(sent[0]);
    CHECK_FALSE(piece.shape);
    REQUIRE(piece.material);
    CHECK(piece.material.value().at(1, 0, 1, 1).value == 7);
    CHECK_FALSE(piece.water);
    CHECK_FALSE(piece.woods);

    // Touched again with nothing changed, or elsewhere, nothing goes.
    sending.changed({&touched, 1});
    CHECK(sending.next(player, lots, *source).empty());
    const PieceSource::Touched away{.node = other, .from = 0, .to = 12};
    sending.changed({&away, 1});
    CHECK(sending.due(player) == 0);
}

TEST_CASE("a piece short of what's asked goes as held, and again once filled under",
          "[server][piece_sending]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 9);
    hog::testing::fill_chunks(generator, store, node, 12);
    hog::jobs::JobSystem jobs{2};
    hog::baked::ChunkFiller filler{generator, store, jobs, {.fill_levels = 3}};
    auto source = PieceSource::open(store, &filler);
    REQUIRE(source.has_value());

    PieceSending sending{grid.depth()};
    const hog::net::ConnectionId player{1};
    sending.join(player);
    const std::vector<PieceSending::Want> wants{{.node = node, .depth = 15, .priority = 0.5}};
    sending.ask(player, wants);
    const auto first = sending.next(player, lots, *source);
    REQUIRE(first.size() == 1);
    CHECK_FALSE(
        decoded(first[0]).shape.value().same_as(hog::globe::fill(generator, grid, node, 6).shape));
    CHECK(sending.waiting(player) == 1);

    while (!filler.idle()) {
        const auto touched = source->pump(true);
        REQUIRE(touched.has_value());
        sending.changed(*touched);
    }
    CHECK(sending.waiting(player) == 0);
    const auto filled = sending.next(player, lots, *source);
    REQUIRE(filled.size() == 1);
    const hog::grid::Piece piece = decoded(filled[0]);
    CHECK(piece.shape.value().same_as(hog::globe::fill(generator, grid, node, 6).shape));
    CHECK(piece.material);
    CHECK(sending.waiting(player) == 0);
    CHECK(sending.due(player) == 0);
}
