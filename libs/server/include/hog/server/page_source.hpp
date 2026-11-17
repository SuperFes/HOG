#pragma once

#include <hog/baked/filling.hpp>
#include <hog/baked/page_store.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/page.hpp>
#include <hog/grid/tree.hpp>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace hog::server {

// The grid pages the server sends, read from the store as deep as asked. A page it doesn't hold
// whole as this generator makes them, or not that deep, is asked of the filler, where there is
// one; without one, the store is all there is.
class PageSource {
public:
    [[nodiscard]] static Result<PageSource> open(baked::PageStore& store,
                                                 baked::Filler* filler = nullptr);

    // The page rooted at `root`, every tree as held now and cut to `levels` where deeper; none
    // while it's first being filled, where nothing fills it, or where the page over it has nothing
    // finer to fill it with. Held shallower, it's what's held while it's filled deeper.
    [[nodiscard]] std::optional<grid::Page> page(grid::Key root, std::uint8_t levels);

    // The page's shape tree, as `page` gives it.
    [[nodiscard]] std::optional<grid::Tree<grid::Shape>> shape(grid::Key root, std::uint8_t levels);

    // Writes what the filler finished and moves on to the store as it is now, giving the roots of
    // the pages it wrote. With `wait`, first waits for a fill under way where none has finished.
    [[nodiscard]] Result<std::vector<grid::Key>> pump(bool wait = false);

private:
    PageSource(baked::PageStore::Snapshot snapshot, baked::Filler* filler)
        : snapshot_{std::move(snapshot)}, filler_{filler} {}

    void ask(grid::Key root, std::uint8_t levels);

    baked::PageStore::Snapshot snapshot_;
    baked::Filler* filler_;
};

} // namespace hog::server
