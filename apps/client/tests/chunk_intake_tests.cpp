#include "held_chunks.hpp"
#include "sloped_ground.hpp"

#include <hog/client/chunk_cache.hpp>
#include <hog/client/chunk_intake.hpp>
#include <hog/client/held_pieces.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/testing/temporary_directory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using hog::client::ChunkCache;
using hog::client::ChunkIntake;

namespace {

// The chunks of the sloped ground's pieces, as the server sends them.
std::vector<hog::protocol::SentChunk> sloped_chunks(const hog::grid::Grid& grid) {
    std::vector<hog::protocol::SentChunk> chunks =
        hog::testing::chunks_of(*hog::sloped::sloped_root(grid));
    for (hog::protocol::SentChunk& chunk :
         hog::testing::chunks_of(*hog::sloped::sloped_finely(grid))) {
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

// What the held pieces hold, as a snapshot's entries keyed by root, trees coded.
std::vector<std::pair<hog::grid::Key, std::vector<std::byte>>>
holding(const hog::client::HeldPieces& held) {
    std::vector<std::pair<hog::grid::Key, std::vector<std::byte>>> found;
    for (const auto& [root, piece] : held.entries()) {
        found.emplace_back(root, piece->shape ? piece->shape->encode() : std::vector<std::byte>{});
    }
    return found;
}

} // namespace

TEST_CASE("chunks taken in off the thread that draws hold what taking them there does",
          "[client][chunk_intake]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    hog::jobs::JobSystem jobs{2};
    const std::vector<hog::protocol::SentChunk> chunks = sloped_chunks(grid);
    REQUIRE(chunks.size() > 1);

    hog::client::HeldPieces direct{grid};
    for (const hog::protocol::SentChunk& chunk : chunks) {
        REQUIRE(direct.take(chunk));
    }

    const hog::testing::TemporaryDirectory directory;
    hog::client::HeldPieces held{grid};
    {
        ChunkIntake intake{jobs,
                           ChunkCache::open(directory.path() / "cache.mdb", 1U << 30U).value()};
        intake.sent(chunks);
        ChunkIntake::Taken taken = intake.finish();
        CHECK(taken.failed.empty());
        CHECK(taken.kept.empty());
        REQUIRE(taken.chunks.size() == chunks.size());
        for (hog::client::DecodedChunk& chunk : taken.chunks) {
            const auto ok = held.take(std::move(chunk));
            REQUIRE(ok);
        }
        CHECK(intake.waiting() == 0);
    }
    CHECK(holding(held) == holding(direct));

    // Kept as they came, they're read back for a node over them all, to be checked.
    ChunkIntake again{jobs, ChunkCache::open(directory.path() / "cache.mdb", 1U << 30U).value()};
    const ChunkCache::Wanted all{.node = hog::grid::Key{}, .depth = grid.depth()};
    again.look_up({all});
    ChunkIntake::Taken read = again.finish();
    CHECK(read.failed.empty());
    REQUIRE(read.looked.size() == 1);
    CHECK(read.chunks.size() == chunks.size());
    CHECK(read.kept.size() == chunks.size());
    hog::client::HeldPieces from_cache{grid};
    for (hog::client::DecodedChunk& chunk : read.chunks) {
        const auto ok = from_cache.take(std::move(chunk));
        REQUIRE(ok);
    }
    CHECK(holding(from_cache) == holding(direct));
}

TEST_CASE("what's looked up with no cache comes back looked up and empty",
          "[client][chunk_intake]") {
    hog::jobs::JobSystem jobs{2};
    ChunkIntake intake{jobs, std::nullopt};
    CHECK_FALSE(intake.caching());
    intake.look_up({ChunkCache::Wanted{.node = hog::grid::Key{}, .depth = 3}});
    const ChunkIntake::Taken taken = intake.finish();
    CHECK(taken.looked.size() == 1);
    CHECK(taken.chunks.empty());
}

TEST_CASE("a flood of chunks is decoded a few jobs at a time and comes back in order",
          "[client][chunk_intake]") {
    const hog::grid::Grid grid{1000.0, 1.0};
    hog::jobs::JobSystem jobs{2};
    const std::vector<hog::protocol::SentChunk> once = sloped_chunks(grid);
    // Far more slices than jobs may be in flight, each told apart by its version.
    std::vector<hog::protocol::SentChunk> flood;
    for (std::uint64_t round = 1; flood.size() < 20'000; ++round) {
        for (hog::protocol::SentChunk chunk : once) {
            chunk.version = round;
            flood.push_back(std::move(chunk));
        }
    }
    ChunkIntake intake{jobs, std::nullopt};
    intake.sent(flood);
    std::vector<std::uint64_t> versions;
    while (intake.waiting() > 0) {
        for (const hog::client::DecodedChunk& chunk : intake.take().chunks) {
            versions.push_back(chunk.version);
        }
    }
    REQUIRE(versions.size() == flood.size());
    CHECK(std::ranges::is_sorted(versions));
}
