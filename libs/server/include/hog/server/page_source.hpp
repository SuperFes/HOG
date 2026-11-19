#pragma once

#include <hog/baked/filling.hpp>
#include <hog/baked/page_store.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/page.hpp>
#include <hog/grid/tree.hpp>

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::server {

// A page as sent to a player holding none of it: cut as deep as asked or as it's held if that's
// shallower, its bytes as `grid::Page::encode` codes them, and the water bodies its water marks.
struct Answer {
    grid::Page page;
    std::vector<std::byte> bytes;
    std::vector<grid::Water::Value> bodies;

    [[nodiscard]] std::uint8_t levels() const { return page.shape.value().tree.levels(); }
};

// The grid pages the server sends, read from the store as deep as asked. A page it doesn't hold
// whole as this generator makes them, or not that deep, is asked of the filler at the rank given,
// where there is one; without one, the store is all there is.
//
// Answers are kept for whoever asks next, up to a budget of bytes, the least lately asked for let
// go first. One is let go too as soon as any of its page's trees is held at another version than
// it was made from, whoever wrote it, which each asking checks without decoding anything; and at
// once when the filler writes its page.
class PageSource {
public:
    static constexpr std::size_t default_budget = std::size_t{256} << 20U;

    [[nodiscard]] static Result<PageSource> open(baked::PageStore& store,
                                                 baked::Filler* filler = nullptr,
                                                 std::size_t budget = default_budget);

    // The page rooted at `root`, every tree as held now and cut to `levels` where deeper; none
    // while it's first being filled, where nothing fills it, or where the page over it has nothing
    // finer to fill it with. Held shallower, it's what's held while it's filled deeper.
    [[nodiscard]] std::optional<grid::Page> page(grid::Key root, std::uint8_t levels,
                                                 baked::Rank rank = {});

    // The same as an answer, kept and shared while it's current.
    [[nodiscard]] std::shared_ptr<const Answer> answer(grid::Key root, std::uint8_t levels,
                                                       baked::Rank rank = {});

    // The page's shape tree, as `page` gives it.
    [[nodiscard]] std::optional<grid::Tree<grid::Shape>> shape(grid::Key root, std::uint8_t levels,
                                                               baked::Rank rank = {});

    // Writes what the filler finished and moves on to the store as it is now, giving the roots of
    // the pages it wrote, and of those it found held deep enough already. With `wait`, first waits
    // for a fill under way where none has finished.
    [[nodiscard]] Result<std::vector<grid::Key>> pump(bool wait = false);

    struct Kept {
        std::size_t pages = 0;
        std::size_t bytes = 0;
        std::size_t found = 0;  // answers asked for and kept
        std::size_t made = 0;   // answers made afresh
        std::size_t let_go = 0; // pages let go, for the budget or as no longer current
    };
    [[nodiscard]] Kept kept() const noexcept {
        Kept counts = counts_;
        counts.pages = kept_.size();
        return counts;
    }

private:
    PageSource(baked::PageStore::Snapshot snapshot, baked::Filler* filler, std::size_t budget)
        : snapshot_{std::move(snapshot)}, filler_{filler}, budget_{budget} {}

    void ask(grid::Key root, std::uint8_t levels, baked::Rank rank);
    void let_go(grid::Key root);

    struct Hash {
        [[nodiscard]] std::size_t operator()(grid::Key key) const noexcept {
            return static_cast<std::size_t>(grid::code(key));
        }
    };
    // What's kept of a page: the versions it was read at, how deep it's held, its answers by how
    // deep each is cut, their bytes counted against the budget, and where it stands in `recent_`.
    struct Page {
        baked::PageStore::Versions versions;
        std::uint8_t levels = 0;
        std::map<std::uint8_t, std::shared_ptr<const Answer>> answers;
        std::size_t bytes = 0;
        std::list<grid::Key>::iterator recent;
    };

    baked::PageStore::Snapshot snapshot_;
    baked::Filler* filler_;
    std::size_t budget_;
    std::unordered_map<grid::Key, Page, Hash> kept_;
    std::list<grid::Key> recent_; // the latest asked for first
    Kept counts_;
};

} // namespace hog::server
