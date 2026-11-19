#pragma once

#include <hog/baked/page_store.hpp>
#include <hog/core/result.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/grid_fill.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/jobs/job_system.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hog::baked {

// What a page store of the generator's world is.
[[nodiscard]] Baking baking_of(const globe::Generator& generator);

// Writes each of `pages` as filled, in one batch.
[[nodiscard]] Result<void> write_filled(PageStore& store, std::span<const grid::Key> pages,
                                        std::span<const std::optional<globe::Filled>> filled);

// How soon a node is wanted: those of a later round first, then the higher priority.
struct Rank {
    std::uint64_t round = 0;
    double priority = 0.0;

    friend constexpr auto operator<=>(const Rank&, const Rank&) noexcept = default;
};

// Fills the pages the nodes asked for need, as the store doesn't hold them as this generator makes
// them yet, on the job system's workers, and writes them on a thread of its own, so `pump` never
// waits on the disk; `pump` says which were written. A node is answered by the pages over it down
// to the one holding its depth, or to one whose leaf over it isn't open: pages under such a leaf
// would only repeat it. A page is filled whole, so it's filled once however deep it's later wanted;
// one held shallower than a node asked for needs is filled again whole. Fills start in rank order,
// a page taking the highest rank of the nodes waiting on it.
class Filler {
public:
    // Made on a thread of `jobs`' pool, as `pump` is called.
    Filler(const globe::Generator& generator, PageStore& store, jobs::JobSystem& jobs);
    Filler(const Filler&) = delete;
    Filler& operator=(const Filler&) = delete;
    Filler(Filler&&) = delete;
    Filler& operator=(Filler&&) = delete;
    // Waits for fills under way and any write under way, writing no more.
    ~Filler();

    // From any thread.
    void want(grid::Key node, Rank rank = {});

    // Writes the fills that have finished, together, and starts those the nodes wanted need next,
    // giving the roots of the pages written. With `wait`, first waits for a fill under way where
    // none has finished. From one thread at a time.
    [[nodiscard]] Result<std::vector<grid::Key>> pump(bool wait = false);

    // The roots of the pages found held as deep as the nodes asked for need since last taken, with
    // nothing to fill: another writer may have filled them since they were last read. From the
    // thread calling `pump`.
    [[nodiscard]] std::vector<grid::Key> take_ready();

    // Whether nothing is wanted or being filled. From the thread calling `pump`.
    [[nodiscard]] bool idle() const;

    // The pages waiting their turn to be filled, the first to start first. From the thread
    // calling `pump`.
    [[nodiscard]] std::vector<grid::Key> queued() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hog::baked
