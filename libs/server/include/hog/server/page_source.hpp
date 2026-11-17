#pragma once

#include <hog/baked/filling.hpp>
#include <hog/baked/page_store.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/page.hpp>

#include <optional>
#include <utility>
#include <vector>

namespace hog::server {

// The grid pages the server sends, read from the store. A page it doesn't hold whole as this
// generator makes them is asked of the filler, where there is one; without one, the store is all
// there is.
class PageSource {
public:
    [[nodiscard]] static Result<PageSource> open(baked::PageStore& store,
                                                 baked::Filler* filler = nullptr);

    // The page rooted at `root`, every tree as held now; none while it's being filled, where
    // nothing fills it, or where the page over it has nothing finer to fill it with.
    [[nodiscard]] std::optional<grid::Page> page(grid::Key root);

    // Writes what the filler finished and moves on to the store as it is now, giving the roots of
    // the pages it wrote. With `wait`, first waits for a fill under way where none has finished.
    [[nodiscard]] Result<std::vector<grid::Key>> pump(bool wait = false);

private:
    PageSource(baked::PageStore::Snapshot snapshot, baked::Filler* filler)
        : snapshot_{std::move(snapshot)}, filler_{filler} {}

    baked::PageStore::Snapshot snapshot_;
    baked::Filler* filler_;
};

} // namespace hog::server
