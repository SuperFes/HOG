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

// Fills the pages the nodes asked for need, as the store doesn't hold them as this generator makes
// them yet, on the job system's workers, and writes them from `pump`, which says which were
// written. A node is answered by the pages over it down to the one holding its depth, or to one
// whose leaf over it isn't open: pages under such a leaf would only repeat it. A page is filled
// only as deep as the nodes asked for under it reach, and filled deeper once one reaches further.
class Filler {
public:
    // Made on a thread of `jobs`' pool, as `pump` is called.
    Filler(const globe::Generator& generator, PageStore& store, jobs::JobSystem& jobs);
    Filler(const Filler&) = delete;
    Filler& operator=(const Filler&) = delete;
    Filler(Filler&&) = delete;
    Filler& operator=(Filler&&) = delete;
    // Waits for fills under way, writing none of them.
    ~Filler();

    // From any thread.
    void want(grid::Key node);

    // Writes the fills that have finished, together, and starts those the nodes wanted need next,
    // giving the roots of the pages written. With `wait`, first waits for a fill under way where
    // none has finished. From one thread at a time.
    [[nodiscard]] Result<std::vector<grid::Key>> pump(bool wait = false);

    // Whether nothing is wanted or being filled. From the thread calling `pump`.
    [[nodiscard]] bool idle() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hog::baked
