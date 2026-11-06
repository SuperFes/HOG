#include <hog/chunks/globe_stream.hpp>
#include <hog/client/chunk_cache.hpp>
#include <hog/globe/cell.hpp>
#include <hog/protocol/messages.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <vector>

namespace {

using hog::client::ChunkCache;
using hog::globe::Cell;
using hog::protocol::Chunk;
using hog::protocol::ChunkPlan;
using hog::protocol::PlannedChunk;

std::uint64_t key_of(Cell cell) {
    return hog::chunks::globe_key(cell);
}

const Cell parent_cell = hog::globe::cell_at_index(6, 100);

// A one-triangle chunk, so it has something to draw.
Chunk chunk_of(std::uint64_t key, std::uint32_t version) {
    Chunk chunk{.key = key, .version = version, .part = 0, .parts = 1, .ground = {}, .water = {}};
    chunk.ground.vertices = {{}, {}, {}};
    chunk.ground.indices = {0, 1, 2};
    return chunk;
}

std::set<std::uint64_t> shown_keys(const hog::chunks::GlobeStream::Changes& changes) {
    std::set<std::uint64_t> keys;
    for (const auto& shown : changes.shown) {
        keys.insert(shown.key);
    }
    return keys;
}

const auto start = hog::MonotonicClock::time_point{};

} // namespace

TEST_CASE("the chunks of the plan not held are asked for, nearest first, a window at a time",
          "[client][chunks]") {
    ChunkCache cache{100};
    ChunkPlan plan;
    std::vector<std::uint64_t> keys;
    for (std::uint64_t index = 0; index < 40; ++index) {
        keys.push_back(key_of(hog::globe::cell_at_index(6, index)));
        plan.wanted.push_back({.key = keys.back(), .version = 1});
    }
    cache.plan(plan);
    const std::vector<std::uint64_t> first = cache.to_ask(start);
    REQUIRE(first.size() == ChunkCache::window);
    CHECK(first.front() == keys.front());
    CHECK(first.back() == keys[ChunkCache::window - 1]);
    // Nothing more while they're waited on.
    CHECK(cache.to_ask(start).empty());
    // As some come, others are asked for.
    cache.receive(chunk_of(keys[0], 1));
    cache.receive(chunk_of(keys[1], 1));
    const std::vector<std::uint64_t> more = cache.to_ask(start);
    REQUIRE(more.size() == 2);
    CHECK(more.front() == keys[ChunkCache::window]);
    // Asked for again if they never come.
    const auto later = start + ChunkCache::ask_again + std::chrono::seconds{1};
    CHECK(cache.to_ask(later).size() == ChunkCache::window);
}

TEST_CASE("a chunk held at its planned version isn't asked for, but a newer one is",
          "[client][chunks]") {
    ChunkCache cache{100};
    const std::uint64_t key = key_of(parent_cell);
    cache.plan({.wanted = {{.key = key, .version = 1}}});
    CHECK(cache.to_ask(start) == std::vector<std::uint64_t>{key});
    cache.receive(chunk_of(key, 1));
    CHECK(cache.to_ask(start).empty());
    // Meshed again.
    cache.plan({.wanted = {{.key = key, .version = 2}}});
    CHECK(cache.to_ask(start) == std::vector<std::uint64_t>{key});
    // A late older one doesn't replace the newer.
    cache.receive(chunk_of(key, 2));
    cache.receive(chunk_of(key, 1));
    CHECK(cache.to_ask(start).empty());
}

TEST_CASE("the plan is drawn from what's held, and kept drawn with the version changed",
          "[client][chunks]") {
    ChunkCache cache{100};
    const std::uint64_t key = key_of(parent_cell);
    cache.plan({.wanted = {{.key = key, .version = 1}}});
    CHECK(cache.changes().shown.empty());
    cache.receive(chunk_of(key, 1));
    CHECK(shown_keys(cache.changes()) == std::set<std::uint64_t>{key});
    CHECK(cache.changes().shown.empty());
    CHECK(cache.drawn() == 1);
    // A newer one is drawn in place; the old stays until it comes.
    cache.plan({.wanted = {{.key = key, .version = 2}}});
    CHECK(cache.changes().shown.empty());
    cache.receive(chunk_of(key, 2));
    CHECK(shown_keys(cache.changes()) == std::set<std::uint64_t>{key});
}

TEST_CASE("a chunk is drawn until what's planned in its place is there", "[client][chunks]") {
    ChunkCache cache{100};
    const std::uint64_t coarse = key_of(parent_cell);
    const auto children = hog::globe::children(parent_cell);
    cache.plan({.wanted = {{.key = coarse, .version = 1}}});
    cache.receive(chunk_of(coarse, 1));
    REQUIRE(shown_keys(cache.changes()).size() == 1);
    // The eye comes near: four finer chunks are wanted instead.
    ChunkPlan finer;
    for (const Cell child : children) {
        finer.wanted.push_back({.key = key_of(child), .version = 1});
    }
    cache.plan(finer);
    // Nothing of them yet: the coarse stays drawn, not taken away.
    const auto none = cache.changes();
    CHECK(none.gone.empty());
    CHECK(cache.drawn() == 1);
    // Some of them: still not all.
    cache.receive(chunk_of(key_of(children[0]), 1));
    cache.receive(chunk_of(key_of(children[1]), 1));
    CHECK(cache.changes().gone.empty());
    // All of them: the coarse goes.
    cache.receive(chunk_of(key_of(children[2]), 1));
    cache.receive(chunk_of(key_of(children[3]), 1));
    const auto done = cache.changes();
    CHECK(done.gone == std::vector<std::uint64_t>{coarse});
    CHECK(cache.drawn() == 4);
}

TEST_CASE("a chunk wanted and not held is drawn from the finer chunks held there, if they cover it",
          "[client][chunks]") {
    ChunkCache cache{100};
    const std::uint64_t coarse = key_of(parent_cell);
    const auto children = hog::globe::children(parent_cell);
    // Held from when the eye was near.
    cache.plan({.wanted = {{.key = key_of(children[0]), .version = 1},
                           {.key = key_of(children[1]), .version = 1},
                           {.key = key_of(children[2]), .version = 1},
                           {.key = key_of(children[3]), .version = 1}}});
    for (const Cell child : children) {
        cache.receive(chunk_of(key_of(child), 1));
    }
    REQUIRE(cache.changes().shown.size() == 4);
    // Gone far off: the coarse is wanted and isn't held, and the finer cover it.
    cache.plan({.wanted = {{.key = coarse, .version = 1}}});
    const auto changes = cache.changes();
    CHECK(changes.gone.empty());
    CHECK(cache.drawn() == 4);
    // Asked for, though.
    CHECK(cache.to_ask(start) == std::vector<std::uint64_t>{coarse});
    // Once it's there, it takes their place.
    cache.receive(chunk_of(coarse, 1));
    const auto after = cache.changes();
    CHECK(shown_keys(after) == std::set<std::uint64_t>{coarse});
    CHECK(after.gone.size() == 4);
}

TEST_CASE(
    "the least recently planned chunks are let go past what's held, never what's planned or drawn",
    "[client][chunks]") {
    ChunkCache cache{3};
    std::vector<std::uint64_t> keys;
    for (std::uint64_t index = 0; index < 6; ++index) {
        keys.push_back(key_of(hog::globe::cell_at_index(6, index)));
    }
    // Planned one at a time, each held.
    for (std::size_t at = 0; at < 4; ++at) {
        cache.plan({.wanted = {{.key = keys[at], .version = 1}}});
        cache.receive(chunk_of(keys[at], 1));
        static_cast<void>(cache.changes());
    }
    // Four were planned, one at a time; one's drawn and planned; of the rest, room for two more.
    CHECK(cache.held() == 3);
    // The newest is held, and the oldest let go: asked for again if wanted.
    cache.plan({.wanted = {{.key = keys[0], .version = 1}}});
    CHECK(cache.to_ask(start) == std::vector<std::uint64_t>{keys[0]});
    cache.plan({.wanted = {{.key = keys[3], .version = 1}}});
    CHECK(cache.to_ask(start).empty());
}
