#pragma once

#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/baked/filling.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/piece.hpp>
#include <hog/grid/z_order.hpp>

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace hog::server {

// A piece as sent to a client holding none of it.
struct PieceAnswer {
    // What each kind's tree was read from: a hash of its chunks and their versions, so it changes
    // whenever any of them does.
    struct Stamps {
        std::uint64_t shape = 0;
        std::uint64_t material = 0;
        std::uint64_t water = 0;
        std::uint64_t woods = 0;

        friend constexpr bool operator==(const Stamps&, const Stamps&) noexcept = default;
    };

    grid::Piece piece;
    // As `grid::Piece::encode` codes it.
    std::vector<std::byte> bytes;
    // The water bodies its water tree marks.
    std::vector<grid::Water::Value> bodies;
    Stamps stamps;
    // Whether it goes down to the depth asked wherever there's more under it; short, it's what's
    // held while the rest is filled.
    bool whole = false;
};

// The pieces the server sends, read from the chunk store down to the depth asked. Where the store
// doesn't hold all of a piece yet, what it holds goes, and the rest is asked of the filler at the
// rank given, where there is one; without one, the store is all there is.
//
// Answers are kept for whoever asks next, up to a budget of bytes, the least lately asked for let
// go first. One is let go too as soon as any chunk it was read from is held at another version,
// whoever wrote it, which each asking checks without decoding anything; and at once when the
// filler writes under it.
class PieceSource {
public:
    static constexpr std::size_t default_budget = std::size_t{256} << 20U;

    // The nodes under `node`, from `from` deep to `to`, that a write may have changed.
    struct Touched {
        grid::Key node;
        std::uint8_t from = 0;
        std::uint8_t to = 0;

        friend constexpr bool operator==(const Touched&, const Touched&) noexcept = default;
    };
    // Whether any of what's touched lies in the piece under `node` down to `depth`.
    [[nodiscard]] static bool overlaps(Touched touched, grid::Key node,
                                       std::uint8_t depth) noexcept;

    [[nodiscard]] static Result<PieceSource> open(baked::ChunkStore& store,
                                                  baked::ChunkFiller* filler = nullptr,
                                                  std::size_t budget = default_budget);

    // `node` down to `depth`, as held now; none where nothing of it is held yet, or the node lies
    // outside the grid. A depth past the finest is taken as the finest, one over the node as the
    // node's.
    [[nodiscard]] std::shared_ptr<const PieceAnswer> answer(grid::Key node, std::uint8_t depth,
                                                            baked::Rank rank = {});

    // Writes what the filler finished and moves on to the store as it is now, giving what it
    // wrote, and the asks it found held already. With `wait`, first waits for a fill or a write
    // under way.
    [[nodiscard]] Result<std::vector<Touched>> pump(bool wait = false);

    struct Kept {
        std::size_t pieces = 0;
        std::size_t bytes = 0;
        std::size_t found = 0;  // answers asked for and kept
        std::size_t made = 0;   // answers made afresh
        std::size_t let_go = 0; // for the budget or as no longer current
    };
    [[nodiscard]] Kept kept() const noexcept {
        Kept counts = counts_;
        counts.pieces = recent_.size();
        return counts;
    }

private:
    using Asked = std::pair<grid::Key, std::uint8_t>;

    // The chunks each kind of an answer was read from.
    struct Read {
        std::vector<baked::Chunk> shape;
        std::vector<baked::Chunk> material;
        std::vector<baked::Chunk> water;
        std::vector<baked::Chunk> woods;
    };
    // What's kept of an answer: what it was read from, its bytes counted against the budget, and
    // where it stands in `recent_`.
    struct Entry {
        std::shared_ptr<const PieceAnswer> answer;
        Read read;
        std::size_t bytes = 0;
        std::list<Asked>::iterator recent;
    };

    PieceSource(baked::ChunkStore& store, baked::ChunkStore::Snapshot snapshot,
                baked::ChunkFiller* filler, std::size_t budget)
        : store_{&store}, snapshot_{std::move(snapshot)}, filler_{filler}, budget_{budget} {}

    [[nodiscard]] bool current(const Read& read) const;
    [[nodiscard]] std::shared_ptr<const PieceAnswer> made(grid::Key node, std::uint8_t depth);
    void let_go(grid::Key node, std::uint8_t depth);

    baked::ChunkStore* store_;
    baked::ChunkStore::Snapshot snapshot_;
    baked::ChunkFiller* filler_;
    std::size_t budget_;
    // By node, then by depth.
    grid::ZMap<std::map<std::uint8_t, Entry>> kept_;
    std::list<Asked> recent_; // the latest asked for first
    Kept counts_;
};

} // namespace hog::server
