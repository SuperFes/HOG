#pragma once

#include <hog/client/held_pieces.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/view.hpp>
#include <hog/grid/z_order.hpp>
#include <hog/protocol/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace hog::client {

// A node to ask the server for, the depth to have it down to, and how much it's wanted: the
// nearer the eye the more, however big, the finest size added to its distance so the node
// holding the eye has one, less where it lies behind where the eye looks.
struct PieceWish {
    grid::Key node;
    std::uint8_t depth = 0;
    double priority = 0.0;

    friend constexpr bool operator==(const PieceWish&, const PieceWish&) noexcept = default;
};

// Levels one ask spans at most, so it carries a few thousand nodes of ground near or far.
inline constexpr std::uint8_t region_levels = 5;

// How much less a node wholly behind where the eye looks is wanted than one ahead.
inline constexpr double behind_eye = 0.25;

// The depth nodes in `node` should be seen to from `metres` away: the first, from the node's own,
// whose nodes span no more than `view` allows; the finest at most.
[[nodiscard]] std::uint8_t depth_wanted(const grid::Grid& grid, const grid::View& view,
                                        grid::Key node, double metres) noexcept;

// What to ask for to see the world from `eye`, metres from the planet's centre, looking along
// `facing` where given, that `held` doesn't cover yet. From the root down through what's held: a
// node wanted no more than `region_levels` under it is one region, asked down to the depth it
// should be seen to; one wanted deeper is gone down through where it's held, and asked
// `region_levels` deep where it isn't yet, so what's coarse comes first and finer is asked only
// where ground lies. A leaf closed is gone no further, nor an open one the surface misses but
// where the finest is wanted. Regions never overlap. Nearest first, `most` at most.
[[nodiscard]] std::vector<PieceWish> pieces_to_ask(const HeldPieces& held, DVec3 eye,
                                                   std::optional<DVec3> facing,
                                                   const grid::View& view, std::size_t most);

// The nodes asked of the server it hasn't yet told are done, so none is asked twice, and what to
// ask next. As few are asked at once as keep the server busy, so what's asked can follow the eye:
// the rest wait to be asked as those asked are done, most wanted first. One told done short waits
// on fills without holding up the rest.
class PieceRequests {
public:
    static constexpr std::size_t default_window = 512;
    PieceRequests() noexcept = default;
    explicit PieceRequests(std::size_t window) noexcept : window_{window} {}

    // The ask bringing what's open into line with `wishes`, most wanted first: those not open
    // while there's room, or open less deep than wished, each with how deep `held` covers it;
    // and those open that no wish needs any more dropped, where neither it nor a node under it
    // is wished. Nothing to ask if nothing changes.
    [[nodiscard]] std::optional<protocol::AskPieces> ask(const HeldPieces& held,
                                                         std::span<const PieceWish> wishes);
    // Takes in the server's word that these are done.
    void done(std::span<const protocol::DonePiece> done);

    // Asked and not yet told done.
    [[nodiscard]] std::size_t asked() const noexcept { return asked_; }
    // Told done short, waiting on fills.
    [[nodiscard]] std::size_t waiting() const noexcept { return open_.size() - asked_; }
    // Every node open, asked or waiting, as wished when asked, in Z order.
    [[nodiscard]] std::vector<PieceWish> open() const;

private:
    struct Open {
        std::uint8_t depth = 0;
        bool waiting = false;
        double priority = 0.0;
    };
    using Needed = std::set<grid::Key, grid::ZOrder>;
    // The node over `node` open as deep as `depth`, if one is.
    [[nodiscard]] std::optional<grid::Key> open_over(grid::Key node, std::uint8_t depth) const;
    // Into `ask`, what's open and not `needed`, let go of.
    void drop_unneeded(const Needed& needed, protocol::AskPieces& ask);

    std::size_t window_ = default_window;
    std::size_t asked_ = 0;
    grid::ZMap<Open> open_;
};

} // namespace hog::client
