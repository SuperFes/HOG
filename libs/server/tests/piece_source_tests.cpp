#include "piece_world.hpp"

#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/piece.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/server/piece_source.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using hog::grid::Key;
using hog::server::PieceSource;

namespace {

constexpr hog::DVec3 here{0.2, 0.7, -0.68};
constexpr hog::DVec3 there{-0.6, 0.1, 0.79};
constexpr hog::DVec3 nowhere{0.7, -0.5, 0.5};

} // namespace

TEST_CASE("touched nodes overlap a piece only where both reach the same nodes",
          "[server][piece_source]") {
    const Key node{.depth = 6, .x = 20, .y = 33, .z = 7};
    const PieceSource::Touched under{.node = node.child(3).child(1), .from = 9, .to = 11};
    CHECK(PieceSource::overlaps(under, node, 9));
    CHECK(PieceSource::overlaps(under, node, 12));
    CHECK_FALSE(PieceSource::overlaps(under, node, 8));
    CHECK_FALSE(PieceSource::overlaps(under, node.child(2), 12));
    const PieceSource::Touched over{.node = node.ancestor(3), .from = 4, .to = 6};
    CHECK(PieceSource::overlaps(over, node, 9));
    CHECK_FALSE(PieceSource::overlaps(over, node.child(0), 9));
    CHECK_FALSE(PieceSource::overlaps({.node = node.ancestor(3), .from = 10, .to = 12}, node, 9));
}

TEST_CASE("answers match the store as deep as it holds them, and nothing where it holds none",
          "[server][piece_source]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 9);
    hog::testing::fill_chunks(generator, store, node, 12);

    auto source = PieceSource::open(store);
    REQUIRE(source.has_value());
    const auto answer = source->answer(node, 12);
    REQUIRE(answer);
    CHECK(answer->whole);
    const hog::globe::Filled filled = hog::globe::fill(generator, grid, node, 3);
    CHECK(answer->piece.root == node);
    CHECK(answer->piece.generator == hog::baked::baking_of(generator).generator);
    CHECK(answer->piece.shape.value().same_as(filled.shape));
    CHECK(answer->piece.material.value().same_as(filled.material));
    CHECK(answer->piece.water.value().same_as(filled.water));
    CHECK(answer->piece.woods.value().same_as(filled.woods));
    const auto decoded = hog::grid::Piece::decode(answer->bytes);
    REQUIRE(decoded.has_value());
    CHECK(decoded->encode() == answer->bytes);
    for (const auto body : filled.water.leaf_values()) {
        CHECK((body == 0 || std::ranges::binary_search(answer->bodies, body)));
    }

    // Asked deeper than held, it's what's held, read from the same chunks, short.
    const auto deeper = source->answer(node, 15);
    REQUIRE(deeper);
    CHECK_FALSE(deeper->whole);
    CHECK(deeper->stamps == answer->stamps);

    CHECK_FALSE(source->answer(hog::testing::ground_key(generator, nowhere, 9), 12));
    CHECK_FALSE(source->answer(Key{.depth = 2, .x = 4}, 4));
}

TEST_CASE("answers are kept for whoever asks next, and let go once a chunk they were read from "
          "changes or for room",
          "[server][piece_source]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 9);
    const Key other = hog::testing::ground_key(generator, there, 9);
    hog::testing::fill_chunks(generator, store, node, 12);
    hog::testing::fill_chunks(generator, store, other, 12);

    auto source = PieceSource::open(store);
    REQUIRE(source.has_value());
    const auto first = source->answer(node, 12);
    REQUIRE(first);
    CHECK(source->answer(node, 12) == first);
    const auto elsewhere = source->answer(other, 12);
    REQUIRE(elsewhere);
    CHECK(source->kept().pieces == 2);
    CHECK(source->kept().made == 2);
    CHECK(source->kept().found == 1);

    // The band under the node written anew: its answer goes, the one elsewhere stays.
    hog::testing::put_material(store, 3, node, 7);
    REQUIRE(source->pump().has_value());
    const auto changed = source->answer(node, 12);
    REQUIRE(changed);
    CHECK(changed != first);
    CHECK(source->kept().let_go == 1);
    CHECK(changed->stamps.material != first->stamps.material);
    CHECK(changed->stamps.shape == first->stamps.shape);
    CHECK_FALSE(changed->piece.material.value().same_as(first->piece.material.value()));
    CHECK(changed->piece.material.value().at(1, 1, 0, 1).value == 7);
    CHECK(source->answer(other, 12) == elsewhere);

    // Room for one answer: each asked for lets go of the other.
    auto tight = PieceSource::open(store, nullptr, 1);
    REQUIRE(tight.has_value());
    REQUIRE(tight->answer(node, 12));
    REQUIRE(tight->answer(other, 12));
    CHECK(tight->kept().pieces == 1);
    REQUIRE(tight->answer(node, 12));
    CHECK(tight->kept().made == 3);
    CHECK(tight->kept().let_go == 2);
}

TEST_CASE("a piece short of what's asked is filled, and let go once the filler writes under it",
          "[server][piece_source]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::grid::Grid grid = hog::globe::grid_of(generator);
    hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 9);
    hog::testing::fill_chunks(generator, store, node, 12);

    hog::jobs::JobSystem jobs{2};
    hog::baked::ChunkFiller filler{generator, store, jobs, {.fill_levels = 3}};
    auto source = PieceSource::open(store, &filler);
    REQUIRE(source.has_value());
    const auto first = source->answer(node, 15);
    REQUIRE(first);
    CHECK_FALSE(first->whole);
    std::vector<PieceSource::Touched> touched;
    while (!filler.idle()) {
        const auto pumped = source->pump(true);
        REQUIRE(pumped.has_value());
        touched.insert(touched.end(), pumped->begin(), pumped->end());
    }
    CHECK(std::ranges::any_of(touched, [&](const PieceSource::Touched& each) {
        return PieceSource::overlaps(each, node, 15);
    }));
    CHECK(source->kept().pieces == 0);

    const auto whole = source->answer(node, 15);
    REQUIRE(whole);
    CHECK(whole->whole);
    const hog::globe::Filled filled = hog::globe::fill(generator, grid, node, 6);
    CHECK(whole->piece.shape.value().same_as(filled.shape));
    CHECK(whole->piece.material.value().same_as(filled.material));
}
