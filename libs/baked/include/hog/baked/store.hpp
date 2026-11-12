#pragma once

#include <hog/baked/node.hpp>
#include <hog/baked/page.hpp>
#include <hog/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hog::baked {

struct Options {
    // How large the file may grow; address space only until it's written.
    std::uint64_t map_bytes = std::uint64_t{1} << 30U;
    // Snapshots open at once, across every thread and process.
    unsigned readers = 1024;
    bool read_only = false;
    // Pages larger than this are stored as their eight halves instead.
    std::size_t page_bytes = std::size_t{256} << 10U;
};

// A node's record: where it stands and its page, read in place.
struct Record {
    Node node{};
    Page page;
};

// One consistent view of the store, as it was when taken or last refreshed: reads take no locks
// and never wait on the writer or other readers. Used on one thread at a time.
class Snapshot {
public:
    Snapshot(Snapshot&&) noexcept;
    Snapshot& operator=(Snapshot&&) noexcept;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    ~Snapshot();

    // The record holding a point; none where nothing was stored. Its page lies in the store's
    // memory, valid until this snapshot is refreshed or gone.
    [[nodiscard]] std::optional<Record> record(Point p) const;
    // The leaf holding a point; none where nothing was stored.
    [[nodiscard]] std::optional<Leaf> at(Point p) const;
    // Every record within `node`, or holding it, in key order.
    void each(const Node& node, const std::function<void(const Record&)>& visit) const;
    // Moves on to the store as it is now. Long-held snapshots keep old pages from being reused.
    [[nodiscard]] Result<void> refresh();

private:
    friend class Store;
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

    [[nodiscard]] Result<void> set_header(std::span<const std::byte> header);
    // Stores `page` as all of `node`, in place of whatever was there: a record holding more is cut
    // down to the parts beside it, those within it go. Precondition: the page is the node's size.
    [[nodiscard]] Result<void> put(const Node& node, std::span<const std::byte> page);
    // Takes away everything stored in `node`, cutting a record holding more down to the rest.
    [[nodiscard]] Result<void> erase(const Node& node);
    // The record holding a point, as this batch has it so far.
    [[nodiscard]] std::optional<Record> record(Point p) const;

private:
    friend class Store;
    struct Impl;
    explicit Batch(Impl& impl) : impl_{&impl} {}
    Impl* impl_;
};

// The baked world: each face of the planet an octree, its records nodes of any size, each held as
// a page. Records never overlap, and together they cover all that's been baked. Any number of
// snapshots read at once while one batch at a time writes.
class Store {
public:
    // Opens the store in `directory`, making it where missing (unless read only).
    [[nodiscard]] static Result<Store> open(const std::filesystem::path& directory,
                                            const Options& options = {});
    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    ~Store();

    // Empty where none was ever set.
    [[nodiscard]] Result<std::vector<std::byte>> header() const;
    [[nodiscard]] Result<Snapshot> read() const;
    // Runs `body` in one batch, committing it where it succeeds and dropping it where it fails.
    [[nodiscard]] Result<void> write(const std::function<Result<void>(Batch&)>& body);

    struct Stats {
        std::size_t records = 0;
        std::size_t bytes = 0; // of the file in use
    };
    [[nodiscard]] Result<Stats> stats() const;

private:
    struct Impl;
    explicit Store(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace hog::baked
