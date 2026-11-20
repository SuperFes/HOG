#include <hog/client/chunk_cache.hpp>
#include <hog/grid/key.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

using hog::client::ChunkCache;
using hog::grid::Key;
using hog::protocol::SentChunk;

namespace {

SentChunk chunk_at(std::uint8_t kind, std::uint8_t band, Key root, std::uint64_t version,
                   std::size_t size = 4) {
    return {.kind = kind,
            .band = band,
            .depth = root.depth,
            .x = root.x,
            .y = root.y,
            .z = root.z,
            .version = version,
            .tree = std::vector<std::byte>(size, static_cast<std::byte>(version))};
}

std::vector<SentChunk> sorted(std::vector<SentChunk> chunks) {
    std::ranges::sort(chunks, [](const SentChunk& a, const SentChunk& b) {
        return std::tie(a.kind, a.band, a.depth, a.x, a.y, a.z) <
               std::tie(b.kind, b.band, b.depth, b.x, b.y, b.z);
    });
    return chunks;
}

} // namespace

TEST_CASE("a chunk cache gives back what's over a node and under it as deep as wanted, as sent",
          "[client][chunk_cache]") {
    const hog::testing::TemporaryDirectory directory;
    const auto file = directory.path() / "cache.mdb";
    const Key node{.depth = 6, .x = 10, .y = 20, .z = 30};
    const SentChunk over = chunk_at(0, 1, Key{.depth = 3, .x = 1, .y = 2, .z = 3}, 7);
    const SentChunk at = chunk_at(1, 2, node, 8);
    const SentChunk under = chunk_at(2, 2, node.child(3), 9);
    const SentChunk deeper = chunk_at(0, 3, node.child(3).child(1).child(2), 10);
    const SentChunk beside = chunk_at(0, 2, Key{.depth = 6, .x = 11, .y = 20, .z = 30}, 11);
    {
        ChunkCache cache = ChunkCache::open(file, 1U << 20U).value();
        REQUIRE(cache.keep(std::vector{over, at, under, deeper, beside}));
        CHECK(cache.chunks() == 5);
        CHECK(cache.bytes() == 20);
    }
    // Kept between runs.
    ChunkCache cache = ChunkCache::open(file, 1U << 20U).value();
    CHECK(cache.chunks() == 5);
    const std::vector wanted{ChunkCache::Wanted{.node = node, .depth = 8}};
    CHECK(sorted(cache.read(wanted).value()) == sorted({over, at, under}));
    const std::vector deep{ChunkCache::Wanted{.node = node, .depth = 9}};
    CHECK(sorted(cache.read(deep).value()) == sorted({over, at, under, deeper}));

    // Sent again newer, it's kept in place of what was; at version 0, let go.
    const SentChunk newer = chunk_at(1, 2, node, 12, 6);
    REQUIRE(cache.keep(std::vector{newer, chunk_at(2, 2, node.child(3), 0)}));
    CHECK(sorted(cache.read(wanted).value()) == sorted({over, newer}));
    CHECK(cache.chunks() == 4);
    CHECK(cache.bytes() == 18);
}

TEST_CASE("a chunk cache over its budget lets go of what was used longest ago",
          "[client][chunk_cache]") {
    const hog::testing::TemporaryDirectory directory;
    ChunkCache cache = ChunkCache::open(directory.path() / "cache.mdb", 300).value();
    std::vector<Key> roots;
    for (std::uint32_t x = 0; x < 3; ++x) {
        roots.push_back({.depth = 8, .x = x});
        REQUIRE(cache.keep(std::vector{chunk_at(0, 2, roots.back(), x + 1, 100)}));
    }
    // The first read again, the second is now the oldest used.
    const std::vector first{ChunkCache::Wanted{.node = roots[0], .depth = 8}};
    CHECK(cache.read(first).value().size() == 1);
    REQUIRE(cache.keep(std::vector{chunk_at(0, 2, Key{.depth = 8, .x = 3}, 4, 100)}));
    CHECK(cache.bytes() == 300);
    CHECK(cache.chunks() == 3);
    for (std::uint32_t x = 0; x < 4; ++x) {
        const std::vector one{ChunkCache::Wanted{.node = Key{.depth = 8, .x = x}, .depth = 8}};
        CHECK(cache.read(one).value().size() == (x == 1 ? 0U : 1U));
    }
}
