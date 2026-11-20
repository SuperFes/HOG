#pragma once

#include <hog/client/held_pieces.hpp>
#include <hog/client/piece_asking.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/key.hpp>
#include <hog/protocol/messages.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace hog::client {

// The chunks this client's been sent, kept on disk between runs in a database of its own: keyed as
// the server's store keys them, by kind, band and root, each with its version, when it was last
// read or kept, and its tree as sent. Kept under a budget of trees' bytes, letting go of what was
// used longest ago. Used on one thread at a time.
class ChunkCache {
public:
    // Opens the cache in `file`, making it where missing, to keep about `budget` bytes of trees.
    [[nodiscard]] static Result<ChunkCache> open(const std::filesystem::path& file,
                                                 std::uint64_t budget);

    ChunkCache(ChunkCache&&) noexcept;
    ChunkCache& operator=(ChunkCache&&) noexcept;
    ChunkCache(const ChunkCache&) = delete;
    ChunkCache& operator=(const ChunkCache&) = delete;
    ~ChunkCache();

    // Keeps the chunks as sent, each in place of what's kept of it; one of version 0 lets go of
    // it. Then lets go of what was used longest ago while over the budget.
    [[nodiscard]] Result<void> keep(std::span<const protocol::SentChunk> chunks);

    // A node and the depth it's wanted down to.
    struct Wanted {
        grid::Key node;
        std::uint8_t depth = 0;
    };
    // What's kept about each: every chunk of any kind and band whose root holds the node, or lies
    // under it no deeper than the depth wanted, each once, as it was sent. Each read is used now.
    [[nodiscard]] Result<std::vector<protocol::SentChunk>> read(std::span<const Wanted> wanted);

    // Bytes of trees kept, and chunks.
    [[nodiscard]] std::uint64_t bytes() const noexcept;
    [[nodiscard]] std::uint64_t chunks() const noexcept;

private:
    struct Impl;
    explicit ChunkCache(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// The wishes looked up in a cache already, by node and depth.
using Looked = std::set<std::pair<grid::Key, std::uint8_t>, std::less<>>;

// Takes into `held` what `cache` keeps of the wishes not yet `looked` up there, `most` of them at
// most, nearest first, noting them looked up; the chunks taken, for the server to check.
[[nodiscard]] Result<std::vector<protocol::CheckedChunk>>
take_kept(ChunkCache& cache, HeldPieces& held, std::span<const PieceWish> wishes, Looked& looked,
          std::size_t most);

} // namespace hog::client
