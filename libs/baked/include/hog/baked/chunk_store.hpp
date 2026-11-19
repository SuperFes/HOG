#pragma once

#include <hog/baked/page_store.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/tree.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hog::baked {

// The grid's depths taken `height` levels at a time from the root: band b holds the depths
// b·height + 1 to (b + 1)·height, band 0 the root's as well, and the last stops at the finest.
class Bands {
public:
    // Precondition: height > 0.
    Bands(std::uint8_t height, std::uint8_t finest) noexcept;

    [[nodiscard]] std::uint8_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint8_t count() const noexcept { return count_; }
    // Precondition: depth <= finest.
    [[nodiscard]] std::uint8_t band_of(std::uint8_t depth) const noexcept;
    // The deepest a chunk of the band is rooted: all it holds lies under its root.
    [[nodiscard]] std::uint8_t roots(std::uint8_t band) const noexcept;
    // Its deepest depth.
    [[nodiscard]] std::uint8_t foot(std::uint8_t band) const noexcept;

private:
    std::uint8_t height_;
    std::uint8_t finest_;
    std::uint8_t count_;
};

// A chunk held: one band's nodes under its root, at the version it's held at; 0 for one no longer
// held.
struct Chunk {
    std::uint8_t band = 0;
    grid::Key root;
    std::uint64_t version = 0;

    friend constexpr bool operator==(const Chunk&, const Chunk&) noexcept = default;
};

// A chunk held and its tree's bytes as stored, as `grid::Tree::encode` gave them; the bytes are
// the snapshot's, good until it's refreshed or gone.
struct StoredChunk {
    Chunk chunk;
    std::span<const std::byte> tree;
};

// A node's subtree of one kind, read from the chunks it's held in.
template <typename Kind>
struct Piece {
    grid::Tree<Kind> tree;
    std::vector<Chunk> chunks;
};

// The trees of the grid, each kind held in chunks: a chunk holds one band's nodes under one node
// at or over the band's roots, and is split into its root's eight children's only where it would
// pass a byte budget, so ground alike all through stays one small chunk however wide it is. Chunks
// of a band never overlap; any node's is found by one seek. Each chunk keeps the generator version
// that made it and a version, unique across the store, taken whenever its bytes change. Any number
// of snapshots read at once while one batch at a time writes.
class ChunkStore {
public:
    static constexpr std::uint32_t format = 1;

    struct Options {
        // How large the file may grow; address space only until it's written.
        std::uint64_t map_bytes = std::uint64_t{1} << 30U;
        // Snapshots open at once, across every thread and process.
        unsigned readers = 1024;
        bool read_only = false;
        // Fixed for a store once made.
        std::uint8_t band_height = 3;
        // Bytes a chunk holds before it's split; one at its band's roots is never split.
        std::size_t budget = std::size_t{32} << 10U;
    };

    // One consistent view of the store, as it was when taken or last refreshed: reads take no
    // locks and never wait on the writer or other readers. Used on one thread at a time. Chunks
    // another generator made read as not held.
    class Snapshot {
    public:
        Snapshot(Snapshot&&) noexcept;
        Snapshot& operator=(Snapshot&&) noexcept;
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
        ~Snapshot();

        // The subtree under `node` down to `depth`, as deep as the chunks held go: where the
        // bands down to it aren't all held it stops short, its leaves open where there's more
        // detail. A node under a closed leaf is that leaf. None where the node isn't held.
        template <typename Kind>
        [[nodiscard]] std::optional<Piece<Kind>> piece(grid::Key node, std::uint8_t depth) const;
        // The version of the band's chunk rooted at `root`; 0 where none is held.
        template <typename Kind>
        [[nodiscard]] std::uint64_t version(std::uint8_t band, grid::Key root) const;
        // The band's chunks over `node` or under it, in key order.
        template <typename Kind>
        [[nodiscard]] std::vector<Chunk> chunks(std::uint8_t band, grid::Key node) const;
        // The same with their trees' bytes, undecoded.
        template <typename Kind>
        [[nodiscard]] std::vector<StoredChunk> stored(std::uint8_t band, grid::Key node) const;
        // Whether the band's chunks hold all of `node`: one over it, or those under it together.
        template <typename Kind>
        [[nodiscard]] bool holds(std::uint8_t band, grid::Key node) const;
        // Moves on to the store as it is now. Long-held snapshots keep old pages from being
        // reused.
        [[nodiscard]] Result<void> refresh();

    private:
        friend class ChunkStore;
        struct Impl;
        explicit Snapshot(std::unique_ptr<Impl> impl);
        std::unique_ptr<Impl> impl_;
    };

    // Changes made together: every one or none, seen by snapshots taken after the batch commits.
    class Batch {
    public:
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        Batch(Batch&&) = delete;
        Batch& operator=(Batch&&) = delete;
        ~Batch() = default;

        // Holds `tree`, the subtree under `root` down to the band's foot, as the band's nodes
        // under `root`, made by this generator, in place of what was held there; a chunk over it
        // keeps the rest of what it held. Gives each chunk whose bytes changed, and each no
        // longer held. Fails where `root` lies under the band's roots or the tree doesn't reach
        // the band's foot.
        template <typename Kind>
        [[nodiscard]] Result<std::vector<Chunk>> put(std::uint8_t band, grid::Key root,
                                                     const grid::Tree<Kind>& tree);

    private:
        friend class ChunkStore;
        struct Impl;
        explicit Batch(Impl& impl) : impl_{&impl} {}
        Impl* impl_;
    };

    // Opens the store in `directory`, making it where missing (unless read only). Fails on a store
    // of another format, world, grid or band height, or files that aren't a chunk store.
    [[nodiscard]] static Result<ChunkStore> open(const std::filesystem::path& directory,
                                                 const Baking& baking, const Options& options);
    [[nodiscard]] static Result<ChunkStore> open(const std::filesystem::path& directory,
                                                 const Baking& baking) {
        return open(directory, baking, Options{});
    }
    // Opens a store in a new directory of its own under the system's temporary one, removed when
    // the store closes.
    [[nodiscard]] static Result<ChunkStore> scratch(const Baking& baking, const Options& options);
    [[nodiscard]] static Result<ChunkStore> scratch(const Baking& baking) {
        return scratch(baking, Options{});
    }
    ChunkStore(ChunkStore&&) noexcept;
    ChunkStore& operator=(ChunkStore&&) noexcept;
    ChunkStore(const ChunkStore&) = delete;
    ChunkStore& operator=(const ChunkStore&) = delete;
    ~ChunkStore();

    [[nodiscard]] const std::filesystem::path& directory() const noexcept;
    [[nodiscard]] const Bands& bands() const noexcept;
    [[nodiscard]] const Baking& baking() const noexcept;

    [[nodiscard]] Result<Snapshot> read() const;
    // Runs `body` in one batch, committing it where it succeeds and dropping it where it fails.
    [[nodiscard]] Result<void> write(const std::function<Result<void>(Batch&)>& body);

    struct Stats {
        std::size_t chunks = 0;
        std::size_t bytes = 0; // of the file in use
    };
    [[nodiscard]] Result<Stats> stats() const;

private:
    struct Impl;
    explicit ChunkStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

extern template std::optional<Piece<grid::Shape>>
ChunkStore::Snapshot::piece<grid::Shape>(grid::Key, std::uint8_t) const;
extern template std::uint64_t ChunkStore::Snapshot::version<grid::Shape>(std::uint8_t,
                                                                         grid::Key) const;
extern template std::vector<Chunk> ChunkStore::Snapshot::chunks<grid::Shape>(std::uint8_t,
                                                                             grid::Key) const;
extern template bool ChunkStore::Snapshot::holds<grid::Shape>(std::uint8_t, grid::Key) const;
extern template std::vector<StoredChunk> ChunkStore::Snapshot::stored<grid::Shape>(std::uint8_t,
                                                                                   grid::Key) const;
extern template Result<std::vector<Chunk>>
ChunkStore::Batch::put<grid::Shape>(std::uint8_t, grid::Key, const grid::Tree<grid::Shape>&);
extern template std::optional<Piece<grid::Material>>
ChunkStore::Snapshot::piece<grid::Material>(grid::Key, std::uint8_t) const;
extern template std::uint64_t ChunkStore::Snapshot::version<grid::Material>(std::uint8_t,
                                                                            grid::Key) const;
extern template std::vector<Chunk> ChunkStore::Snapshot::chunks<grid::Material>(std::uint8_t,
                                                                                grid::Key) const;
extern template bool ChunkStore::Snapshot::holds<grid::Material>(std::uint8_t, grid::Key) const;
extern template std::vector<StoredChunk>
ChunkStore::Snapshot::stored<grid::Material>(std::uint8_t, grid::Key) const;
extern template Result<std::vector<Chunk>>
ChunkStore::Batch::put<grid::Material>(std::uint8_t, grid::Key, const grid::Tree<grid::Material>&);
extern template std::optional<Piece<grid::Water>>
ChunkStore::Snapshot::piece<grid::Water>(grid::Key, std::uint8_t) const;
extern template std::uint64_t ChunkStore::Snapshot::version<grid::Water>(std::uint8_t,
                                                                         grid::Key) const;
extern template std::vector<Chunk> ChunkStore::Snapshot::chunks<grid::Water>(std::uint8_t,
                                                                             grid::Key) const;
extern template bool ChunkStore::Snapshot::holds<grid::Water>(std::uint8_t, grid::Key) const;
extern template std::vector<StoredChunk> ChunkStore::Snapshot::stored<grid::Water>(std::uint8_t,
                                                                                   grid::Key) const;
extern template Result<std::vector<Chunk>>
ChunkStore::Batch::put<grid::Water>(std::uint8_t, grid::Key, const grid::Tree<grid::Water>&);
extern template std::optional<Piece<grid::Woods>>
ChunkStore::Snapshot::piece<grid::Woods>(grid::Key, std::uint8_t) const;
extern template std::uint64_t ChunkStore::Snapshot::version<grid::Woods>(std::uint8_t,
                                                                         grid::Key) const;
extern template std::vector<Chunk> ChunkStore::Snapshot::chunks<grid::Woods>(std::uint8_t,
                                                                             grid::Key) const;
extern template bool ChunkStore::Snapshot::holds<grid::Woods>(std::uint8_t, grid::Key) const;
extern template std::vector<StoredChunk> ChunkStore::Snapshot::stored<grid::Woods>(std::uint8_t,
                                                                                   grid::Key) const;
extern template Result<std::vector<Chunk>>
ChunkStore::Batch::put<grid::Woods>(std::uint8_t, grid::Key, const grid::Tree<grid::Woods>&);

} // namespace hog::baked
