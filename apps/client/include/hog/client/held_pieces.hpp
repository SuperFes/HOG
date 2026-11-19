#pragma once

#include <hog/core/result.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/piece.hpp>
#include <hog/grid/tree.hpp>
#include <hog/grid/water_bodies.hpp>
#include <hog/grid/z_order.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace hog::client {

// What the held pieces know at a node: the node, or the leaf over it, as deep as the piece
// reaching deepest there holds it, with its depth in the grid, and that piece's root.
template <typename Kind>
struct Known {
    grid::Tree<Kind>::Found found;
    grid::Key root;
};

// The pieces held at a moment, as they were then, read as one tree on another thread while more
// come.
class PiecesSnapshot {
public:
    using Entry = std::pair<grid::Key, std::shared_ptr<const grid::Piece>>;

    PiecesSnapshot(const grid::Grid& grid, std::vector<Entry> pieces);

    [[nodiscard]] const grid::Grid& grid() const noexcept { return grid_; }
    [[nodiscard]] const grid::Piece* piece(grid::Key root) const;
    // As HeldPieces::at.
    template <typename Kind>
    [[nodiscard]] std::optional<Known<Kind>> at(grid::Key node) const;
    // The pieces over the node or any beside it, and those under them that hold, lie in or touch
    // the node's place: all that's read of the ground through it and along its sides. In Z order.
    [[nodiscard]] std::vector<Entry> around(grid::Key node) const;

private:
    [[nodiscard]] const Entry* find(grid::Key root) const;
    // Into `found`, those at the node or over it.
    void over(grid::Key node, std::vector<Entry>& found) const;
    // Into `found`, those under `place` that lie in the node's place or touch it.
    void under_touching(grid::Key node, grid::Key place, std::vector<Entry>& found) const;

    grid::Grid grid_;
    std::vector<Entry> pieces_; // in Z order
    std::uint64_t depths_ = 0;  // a bit for each depth a piece is rooted at
};

// The pieces the server has sent, each as deep and as new as last sent, read as one tree: at a
// node, the piece reaching deepest there answers. And the water bodies their water trees mark.
class HeldPieces {
public:
    explicit HeldPieces(const grid::Grid& grid);

    // Takes in a piece as `grid::Piece::encode` codes it: deeper than the one held at its root,
    // it's held in place of it; as deep or shallower, the trees it carries replace those held.
    // Fails on bytes that aren't a piece, or a piece rooted off the grid or deeper than it.
    [[nodiscard]] Result<void> take(std::span<const std::byte> bytes);
    // Takes in bodies as `grid::WaterBodies::encode` codes them, in place of those of the same ids.
    [[nodiscard]] Result<void> take_bodies(std::span<const std::byte> bytes);

    // What's known at `node`, as Known has it; none where no piece over it holds `Kind`.
    template <typename Kind>
    [[nodiscard]] std::optional<Known<Kind>> at(grid::Key node) const;
    // Whether what's held reaches `depth` all through `node`: a piece over it reaches that deep, a
    // closed leaf holds all of it, or what's under it does, child by child.
    [[nodiscard]] bool covers(grid::Key node, std::uint8_t depth) const;
    // How deep in the grid the piece rooted at `root` reaches; none where it isn't held.
    [[nodiscard]] std::optional<std::uint8_t> reach(grid::Key root) const;

    [[nodiscard]] const grid::Grid& grid() const noexcept { return grid_; }
    [[nodiscard]] const grid::Piece* piece(grid::Key root) const;
    // Every piece held, as it's held now, for a snapshot. In Z order.
    [[nodiscard]] std::vector<PiecesSnapshot::Entry> entries() const;
    [[nodiscard]] std::size_t pieces() const noexcept { return held_.size(); }
    // Of the pieces held, as coded.
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] const grid::WaterBodies& bodies() const noexcept { return *bodies_; }
    // The same, kept as they are for as long as they're wanted.
    [[nodiscard]] std::shared_ptr<const grid::WaterBodies> shared_bodies() const noexcept {
        return bodies_;
    }

private:
    struct Held {
        std::shared_ptr<const grid::Piece> piece;
        std::size_t bytes = 0;
        std::uint8_t reach = 0;
    };

    [[nodiscard]] const Held* find(grid::Key root) const;
    // Whether a piece over the node reaches `depth`, or a closed leaf holds all of it.
    [[nodiscard]] bool held_over(grid::Key node, std::uint8_t depth) const;

    grid::Grid grid_;
    grid::ZMap<Held> held_;
    std::uint64_t depths_ = 0;
    std::size_t bytes_ = 0;
    std::shared_ptr<const grid::WaterBodies> bodies_ = std::make_shared<const grid::WaterBodies>();
};

extern template std::optional<Known<grid::Shape>> HeldPieces::at<grid::Shape>(grid::Key) const;
extern template std::optional<Known<grid::Material>>
    HeldPieces::at<grid::Material>(grid::Key) const;
extern template std::optional<Known<grid::Water>> HeldPieces::at<grid::Water>(grid::Key) const;
extern template std::optional<Known<grid::Woods>> HeldPieces::at<grid::Woods>(grid::Key) const;
extern template std::optional<Known<grid::Shape>> PiecesSnapshot::at<grid::Shape>(grid::Key) const;
extern template std::optional<Known<grid::Material>>
    PiecesSnapshot::at<grid::Material>(grid::Key) const;
extern template std::optional<Known<grid::Water>> PiecesSnapshot::at<grid::Water>(grid::Key) const;
extern template std::optional<Known<grid::Woods>> PiecesSnapshot::at<grid::Woods>(grid::Key) const;

} // namespace hog::client
