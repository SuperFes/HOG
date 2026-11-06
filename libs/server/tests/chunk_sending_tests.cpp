#include <hog/protocol/messages.hpp>
#include <hog/server/chunk_sending.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

namespace {

using hog::protocol::Chunk;

// A chunk's part, with ground enough to be about `kilobytes` bytes long.
Chunk part_of(std::uint64_t key, std::uint8_t part, std::uint8_t parts, std::size_t kilobytes) {
    Chunk chunk{.key = key, .version = 1, .part = part, .parts = parts, .ground = {}, .water = {}};
    chunk.ground.vertices.assign(kilobytes * 1'024 / 22, {});
    chunk.ground.indices = {0, 0, 0};
    return chunk;
}

} // namespace

TEST_CASE("the parts of a chunk bigger than the queue are sent together, not one at a time",
          "[server][chunks]") {
    std::vector<std::size_t> sent;
    const auto send = [&](const std::vector<std::byte>& bytes) {
        sent.push_back(bytes.size());
        return true;
    };
    std::vector<Chunk> offered{part_of(1, 0, 2, 300), part_of(1, 1, 2, 300), part_of(2, 0, 1, 10)};
    const std::vector<Chunk> unsent =
        hog::server::send_chunks(std::move(offered), 0, 256 * 1'024, send);
    // Both parts of the first, over the limit as they are; then the queue is full, so the next
    // waits.
    CHECK(sent.size() == 2);
    REQUIRE(unsent.size() == 1);
    CHECK(unsent[0].key == 2);
}

TEST_CASE("chunks wait while the queue is full, and those after one that waits wait too",
          "[server][chunks]") {
    int sent = 0;
    const auto send = [&](const std::vector<std::byte>&) {
        ++sent;
        return true;
    };
    std::vector<Chunk> offered{part_of(1, 0, 1, 10), part_of(2, 0, 1, 10)};
    const std::vector<Chunk> unsent =
        hog::server::send_chunks(std::move(offered), 300 * 1'024, 256 * 1'024, send);
    CHECK(sent == 0);
    CHECK(unsent.size() == 2);
}

TEST_CASE("a chunk whose send fails is given back whole, and nothing after it is sent",
          "[server][chunks]") {
    int calls = 0;
    const auto send = [&](const std::vector<std::byte>&) { return ++calls < 2; };
    std::vector<Chunk> offered{part_of(1, 0, 2, 10), part_of(1, 1, 2, 10), part_of(2, 0, 1, 10)};
    const std::vector<Chunk> unsent =
        hog::server::send_chunks(std::move(offered), 0, 256 * 1'024, send);
    // The first part went; the second failed; the third waits.
    CHECK(calls == 2);
    REQUIRE(unsent.size() == 2);
    CHECK(unsent[0].key == 1);
    CHECK(unsent[0].part == 1);
    CHECK(unsent[1].key == 2);
}
