#include "piece_world.hpp"

#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/tree.hpp>
#include <hog/net/events.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/server/chunk_sending.hpp>
#include <hog/testing/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>
#include <vector>

using hog::grid::Key;
using hog::server::ChunkSending;

namespace {

constexpr hog::DVec3 here{0.2, 0.7, -0.68};
constexpr hog::DVec3 there{-0.6, 0.1, 0.79};
constexpr std::size_t lots = std::numeric_limits<std::size_t>::max();
const hog::net::ConnectionId player{1};

using Id = std::tuple<std::uint8_t, std::uint8_t, Key>;

Id id_of(const hog::protocol::SentChunk& chunk) {
    return {chunk.kind, chunk.band,
            Key{.depth = chunk.depth, .x = chunk.x, .y = chunk.y, .z = chunk.z}};
}

// Every chunk of each band the store holds over `node` or under it down to `depth`, by kind,
// band and root, with its version and tree's bytes.
std::map<Id, std::pair<std::uint64_t, std::vector<std::byte>>>
stored_about(const hog::baked::ChunkStore& store, Key node, std::uint8_t depth) {
    const auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    std::map<Id, std::pair<std::uint64_t, std::vector<std::byte>>> found;
    const hog::baked::Bands& bands = store.bands();
    for (std::uint8_t band = bands.band_of(node.depth); band <= bands.band_of(depth); ++band) {
        const auto add = [&](std::uint8_t kind, const std::vector<hog::baked::StoredChunk>& each) {
            for (const hog::baked::StoredChunk& chunk : each) {
                found[{kind, band, chunk.chunk.root}] = {chunk.chunk.version,
                                                         {chunk.tree.begin(), chunk.tree.end()}};
            }
        };
        add(0, snapshot->stored<hog::grid::Shape>(band, node));
        add(1, snapshot->stored<hog::grid::Material>(band, node));
        add(2, snapshot->stored<hog::grid::Water>(band, node));
        add(3, snapshot->stored<hog::grid::Woods>(band, node));
    }
    return found;
}

ChunkSending::Next next_of(ChunkSending& sending, const hog::baked::ChunkStore& store,
                           std::size_t room = lots, std::size_t most = lots) {
    const auto snapshot = store.read();
    REQUIRE(snapshot.has_value());
    return sending.next(player, *snapshot, room, most);
}

} // namespace

TEST_CASE("a player is sent the chunks about what it asks for as stored and each once",
          "[server][chunk_sending]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 9);
    const Key other = hog::testing::ground_key(generator, there, 9);
    hog::testing::fill_chunks(generator, store, node, 12);
    hog::testing::fill_chunks(generator, store, other, 12);

    ChunkSending sending{store.bands()};
    sending.join(player);
    const std::vector<ChunkSending::Want> wants{{.node = other, .depth = 12, .priority = 0.5},
                                                {.node = node, .depth = 12, .priority = 0.9}};
    sending.ask(player, wants);
    CHECK(sending.due(player) == 2);
    const ChunkSending::Next next = next_of(sending, store);
    CHECK(next.short_of.empty());
    CHECK(next.too_big == 0);
    CHECK(sending.due(player) == 0);

    // Each as stored, those about the first asked first, and none twice.
    auto expected = stored_about(store, other, 12);
    const std::size_t first = expected.size();
    for (const auto& [key, chunk] : stored_about(store, node, 12)) {
        expected.try_emplace(key, chunk);
    }
    std::map<Id, std::pair<std::uint64_t, std::vector<std::byte>>> sent;
    for (std::size_t n = 0; n < next.chunks.size(); ++n) {
        const hog::protocol::SentChunk& chunk = next.chunks[n];
        CHECK(sent.emplace(id_of(chunk), std::pair{chunk.version, chunk.tree}).second);
        if (n < first) {
            CHECK(stored_about(store, other, 12).contains(id_of(chunk)));
        }
    }
    CHECK(sent == expected);

    // Asked again, all of it went whole: nothing's due.
    sending.ask(player, wants);
    CHECK(sending.due(player) == 0);
    CHECK(next_of(sending, store).chunks.empty());
}

TEST_CASE("what the store holds only part of goes as it is and the rest once it's filled",
          "[server][chunk_sending]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 9);
    hog::testing::fill_chunks(generator, store, node, 9);

    ChunkSending sending{store.bands()};
    sending.join(player);
    const std::vector<ChunkSending::Want> wants{{.node = node, .depth = 12, .priority = 0.5}};
    sending.ask(player, wants);
    const ChunkSending::Next part = next_of(sending, store);
    CHECK_FALSE(part.chunks.empty());
    REQUIRE(part.short_of.size() == 1);
    CHECK(part.short_of.front().want.node == node);
    CHECK(sending.waiting(player) == 1);
    // Asked again while it waits, it's not gone over again.
    sending.ask(player, wants);
    CHECK(sending.due(player) == 0);

    // Filled under it, what's new goes, and nothing it was sent before.
    {
        hog::jobs::JobSystem jobs{2};
        hog::baked::ChunkFiller filler{generator, store, jobs, {.fill_levels = 3}};
        filler.want(node, 12);
        while (!filler.idle()) {
            auto written = filler.pump(true);
            REQUIRE(written.has_value());
            sending.changed(*written);
        }
    }
    CHECK(sending.due(player) == 1);
    const ChunkSending::Next rest = next_of(sending, store);
    CHECK(rest.short_of.empty());
    std::map<Id, std::pair<std::uint64_t, std::vector<std::byte>>> held;
    for (const ChunkSending::Next* each : {&part, &rest}) {
        for (const hog::protocol::SentChunk& chunk : each->chunks) {
            if (chunk.version == 0) {
                held.erase(id_of(chunk));
            } else {
                CHECK(held.insert_or_assign(id_of(chunk), std::pair{chunk.version, chunk.tree})
                          .second);
            }
        }
    }
    CHECK(held == stored_about(store, node, 12));
}

TEST_CASE("a chunk no longer stored is told let go of and one changed goes again",
          "[server][chunk_sending]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 7);
    const Key wider = node.ancestor(6);
    const std::uint8_t band = store.bands().band_of(10);
    hog::testing::put_material(store, band, node, 7);
    ChunkSending sending{store.bands()};
    sending.join(player);
    sending.ask(player, std::vector<ChunkSending::Want>{{.node = node, .depth = 12}});
    const ChunkSending::Next first = next_of(sending, store);
    REQUIRE(first.chunks.size() == 1);
    CHECK(id_of(first.chunks.front()) == Id{1, band, node});

    // Changed, it goes again.
    hog::testing::put_material(store, band, node, 8);
    const std::vector<hog::baked::Region> changed{{.band = band, .root = node}};
    sending.changed(changed);
    const ChunkSending::Next again = next_of(sending, store);
    REQUIRE(again.chunks.size() == 1);
    CHECK(id_of(again.chunks.front()) == Id{1, band, node});
    CHECK(again.chunks.front().version > first.chunks.front().version);

    // Held as part of a chunk over it, the one at the node goes.
    hog::testing::put_material(store, band, wider, 9);
    const std::vector<hog::baked::Region> widened{{.band = band, .root = wider}};
    sending.changed(widened);
    const ChunkSending::Next after = next_of(sending, store);
    std::vector<Id> gone;
    std::vector<Id> came;
    for (const hog::protocol::SentChunk& chunk : after.chunks) {
        (chunk.version == 0 ? gone : came).push_back(id_of(chunk));
        CHECK(chunk.tree.empty() == (chunk.version == 0));
    }
    CHECK(gone == std::vector<Id>{{1, band, node}});
    CHECK(came == std::vector<Id>{{1, band, wider}});
}

TEST_CASE("a message carries no more chunks than allowed with the rest going next",
          "[server][chunk_sending]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 9);
    hog::testing::fill_chunks(generator, store, node, 12);
    ChunkSending sending{store.bands()};
    sending.join(player);
    sending.ask(player, std::vector<ChunkSending::Want>{{.node = node, .depth = 12}});

    std::size_t count = 0;
    std::size_t messages = 0;
    for (;;) {
        const ChunkSending::Next next = next_of(sending, store, 2048, 3);
        if (next.chunks.empty()) {
            break;
        }
        ++messages;
        CHECK(next.chunks.size() <= 3);
        std::size_t bytes = 0;
        for (const hog::protocol::SentChunk& chunk : next.chunks) {
            bytes += chunk.tree.size();
        }
        CHECK((next.chunks.size() == 1 || bytes <= 2048));
        count += next.chunks.size();
    }
    CHECK(messages > 1);
    CHECK(count == stored_about(store, node, 12).size());
    CHECK(sending.due(player) == 0);
}

TEST_CASE("asking again replaces what's due and what no one waits on any more is forgotten",
          "[server][chunk_sending]") {
    const hog::globe::Generator& generator = hog::testing::small_world();
    const hog::baked::ChunkStore store = hog::testing::chunk_scratch(generator);
    const Key node = hog::testing::ground_key(generator, here, 9);
    const Key other = hog::testing::ground_key(generator, there, 9);
    ChunkSending sending{store.bands()};
    sending.join(player);
    sending.ask(player, std::vector<ChunkSending::Want>{{.node = node, .depth = 12}});
    // Nothing stored: it waits on a fill.
    CHECK(next_of(sending, store).short_of.size() == 1);
    CHECK(sending.waiting(player) == 1);
    sending.ask(player, std::vector<ChunkSending::Want>{{.node = other, .depth = 12}});
    CHECK(sending.waiting(player) == 0);
    CHECK(sending.due(player) == 1);

    // The filler finding nothing more to fill under a node makes it whole.
    CHECK(next_of(sending, store).short_of.size() == 1);
    const std::vector<hog::baked::ChunkFiller::Asked> answered{{.node = other, .depth = 12}};
    sending.answered(answered);
    CHECK(sending.waiting(player) == 0);
    sending.ask(player, std::vector<ChunkSending::Want>{{.node = other, .depth = 12}});
    CHECK(sending.due(player) == 0);
    sending.leave(player);
    CHECK(sending.due(player) == 0);
}
