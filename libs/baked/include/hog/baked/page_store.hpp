#pragma once

#include <hog/core/result.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/page.hpp>
#include <hog/grid/tree.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace hog::baked {

// What a page store is of: the world it was baked from (`globe::world_id`), its grid, and the
// generator version that bakes it now. Trees another version made are stale: still held, never
// read as current.
struct Baking {
    std::uint64_t world = 0;
    grid::Grid grid;
    std::uint32_t generator = 0;
};

// Which of a page's trees.
struct Trees {
    bool shape = false;
    bool material = false;
    bool water = false;

    friend constexpr bool operator==(const Trees&, const Trees&) noexcept = default;
};

// The trees of the grid's pages, each held on its own with the generator version that made it
// and its own version, which starts at 1 and steps on whenever its bytes change. Any number of
// snapshots read at once while one batch at a time writes.
class PageStore {
public:
    static constexpr std::uint32_t format = 1;

    struct Options {
        // How large the file may grow; address space only until it's written.
        std::uint64_t map_bytes = std::uint64_t{1} << 30U;
        // Snapshots open at once, across every thread and process.
        unsigned readers = 1024;
        bool read_only = false;
    };

    // A page as held: the trees this generator made, at their versions and stamped with it, and
    // which are held only as another made them.
    struct Read {
        grid::Page page;
        Trees stale;
    };

    // One consistent view of the store, as it was when taken or last refreshed: reads take no
    // locks and never wait on the writer or other readers. Used on one thread at a time.
    class Snapshot {
    public:
        Snapshot(Snapshot&&) noexcept;
        Snapshot& operator=(Snapshot&&) noexcept;
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
        ~Snapshot();

        // The page rooted at `root`; none where none of its trees is held.
        [[nodiscard]] std::optional<Read> page(grid::Key root) const;
        // The root of the deepest page over `key` holding a current tree of `Kind`: the page under
        // it where it's a page's root itself; none where no page over it holds one.
        template <typename Kind>
        [[nodiscard]] std::optional<grid::Key> holder(grid::Key key) const;
        // Moves on to the store as it is now. Long-held snapshots keep old pages from being
        // reused.
        [[nodiscard]] Result<void> refresh();

    private:
        friend class PageStore;
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

        // Holds `tree` as the page at `root`'s, made by this generator, in place of what was held,
        // and gives the version it's held at. Fails where `root` isn't a page's root or the tree
        // goes deeper than its page.
        template <typename Kind>
        [[nodiscard]] Result<std::uint32_t> put(grid::Key root, const grid::Tree<Kind>& tree);

    private:
        friend class PageStore;
        struct Impl;
        explicit Batch(Impl& impl) : impl_{&impl} {}
        Impl* impl_;
    };

    // Opens the store in `directory`, making it where missing (unless read only). Fails on a store
    // of another format, of another world or grid, or files that aren't a page store.
    [[nodiscard]] static Result<PageStore> open(const std::filesystem::path& directory,
                                                const Baking& baking, const Options& options);
    [[nodiscard]] static Result<PageStore> open(const std::filesystem::path& directory,
                                                const Baking& baking) {
        return open(directory, baking, Options{});
    }
    PageStore(PageStore&&) noexcept;
    PageStore& operator=(PageStore&&) noexcept;
    PageStore(const PageStore&) = delete;
    PageStore& operator=(const PageStore&) = delete;
    ~PageStore();

    [[nodiscard]] Result<Snapshot> read() const;
    // Runs `body` in one batch, committing it where it succeeds and dropping it where it fails.
    [[nodiscard]] Result<void> write(const std::function<Result<void>(Batch&)>& body);

    struct Stats {
        std::size_t trees = 0;
        std::size_t bytes = 0; // of the file in use
    };
    [[nodiscard]] Result<Stats> stats() const;

private:
    struct Impl;
    explicit PageStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

extern template std::optional<grid::Key> PageStore::Snapshot::holder<grid::Shape>(grid::Key) const;
extern template std::optional<grid::Key>
    PageStore::Snapshot::holder<grid::Material>(grid::Key) const;
extern template std::optional<grid::Key> PageStore::Snapshot::holder<grid::Water>(grid::Key) const;
extern template Result<std::uint32_t>
PageStore::Batch::put<grid::Shape>(grid::Key, const grid::Tree<grid::Shape>&);
extern template Result<std::uint32_t>
PageStore::Batch::put<grid::Material>(grid::Key, const grid::Tree<grid::Material>&);
extern template Result<std::uint32_t>
PageStore::Batch::put<grid::Water>(grid::Key, const grid::Tree<grid::Water>&);

} // namespace hog::baked
