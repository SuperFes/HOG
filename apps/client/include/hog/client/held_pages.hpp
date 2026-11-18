#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/page.hpp>
#include <hog/grid/tree.hpp>
#include <hog/grid/water_bodies.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hog::client {

// What's known at a node: the node, or the leaf over it, in the finest page holding `Kind` over
// it, with its depth in the grid, and which page that is.
template <typename Kind>
struct KnownAt {
    grid::Tree<Kind>::Found found;
    grid::Key page;
};

// The pages held at a moment, as they were then, for reading on another thread while more come.
class PagesSnapshot {
public:
    using Entry = std::pair<grid::Key, std::shared_ptr<const grid::Page>>;

    PagesSnapshot(const grid::Grid& grid, std::vector<Entry> pages);

    [[nodiscard]] const grid::Grid& grid() const noexcept { return grid_; }
    [[nodiscard]] const grid::Page* page(grid::Key root) const;
    [[nodiscard]] std::shared_ptr<const grid::Page> shared(grid::Key root) const;
    // As HeldPages::at.
    template <typename Kind>
    [[nodiscard]] std::optional<KnownAt<Kind>> at(grid::Key node) const;
    // The pages whose places hold, lie in or touch the node's, to the finest node: all that's
    // read of the ground through it and along its sides. In order of their roots.
    [[nodiscard]] std::vector<Entry> around(grid::Key node) const;

private:
    [[nodiscard]] const Entry* find(grid::Key root) const;
    // Into `found`, those over the node and the nodes beside it.
    void over_around(grid::Key node, std::vector<Entry>& found) const;
    // Into `found`, those finer than the node, in it or touching it.
    void finer_around(grid::Key node, std::vector<Entry>& found) const;

    grid::Grid grid_;
    std::vector<Entry> pages_; // by root
};

// The grid pages the server has sent, each as deep and as new as last sent, and the water bodies
// their water trees mark. What's known at a node is the finest page over it holding that kind.
class HeldPages {
public:
    explicit HeldPages(const grid::Grid& grid);

    // Takes in a page as `grid::Page::encode` codes it: deeper than held, it's held in place of
    // what was; as deep or shallower, the trees it carries replace those held. Fails on bytes that
    // aren't a page, or a page rooted off the grid's pages or deeper than its own.
    [[nodiscard]] Result<void> take(std::span<const std::byte> bytes);
    // Takes in bodies as `grid::WaterBodies::encode` codes them, in place of those of the same ids.
    [[nodiscard]] Result<void> take_bodies(std::span<const std::byte> bytes);

    // What's known at `node`, as KnownAt has it.
    template <typename Kind>
    using Known = KnownAt<Kind>;
    template <typename Kind>
    [[nodiscard]] std::optional<Known<Kind>> at(grid::Key node) const;

    [[nodiscard]] const grid::Grid& grid() const noexcept { return grid_; }
    // The page rooted at `root`, if held: as it's held now, which another taken in replaces.
    [[nodiscard]] const grid::Page* page(grid::Key root) const;
    // The same, kept as it is for as long as it's wanted.
    [[nodiscard]] std::shared_ptr<const grid::Page> shared(grid::Key root) const;
    // Every page held, as it's held now, for a snapshot.
    [[nodiscard]] std::vector<PagesSnapshot::Entry> entries() const;
    [[nodiscard]] std::size_t pages() const noexcept { return count_; }
    // Of the pages held, as coded.
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] const grid::WaterBodies& bodies() const noexcept { return *bodies_; }
    // The same, kept as they are for as long as they're wanted.
    [[nodiscard]] std::shared_ptr<const grid::WaterBodies> shared_bodies() const noexcept {
        return bodies_;
    }

    // Lets go of the pages farthest from `eye`, metres from the planet's centre, until those held
    // come to `budget` bytes or under, but never the top page; gives the roots let go.
    std::vector<grid::Key> evict(DVec3 eye, std::size_t budget);

private:
    struct Held {
        std::shared_ptr<const grid::Page> page;
        std::size_t bytes = 0;
    };
    struct Hash {
        [[nodiscard]] std::size_t operator()(grid::Key key) const noexcept {
            return static_cast<std::size_t>(grid::code(key));
        }
    };
    using Depth = std::unordered_map<grid::Key, Held, Hash>;

    [[nodiscard]] const Held* find(grid::Key root) const;

    grid::Grid grid_;
    std::vector<Depth> held_; // by the depth of the pages' roots
    std::size_t count_ = 0;
    std::size_t bytes_ = 0;
    std::shared_ptr<const grid::WaterBodies> bodies_ = std::make_shared<const grid::WaterBodies>();
};

extern template std::optional<HeldPages::Known<grid::Shape>>
    HeldPages::at<grid::Shape>(grid::Key) const;
extern template std::optional<HeldPages::Known<grid::Material>>
    HeldPages::at<grid::Material>(grid::Key) const;
extern template std::optional<HeldPages::Known<grid::Water>>
    HeldPages::at<grid::Water>(grid::Key) const;
extern template std::optional<KnownAt<grid::Shape>> PagesSnapshot::at<grid::Shape>(grid::Key) const;
extern template std::optional<KnownAt<grid::Water>> PagesSnapshot::at<grid::Water>(grid::Key) const;

} // namespace hog::client
