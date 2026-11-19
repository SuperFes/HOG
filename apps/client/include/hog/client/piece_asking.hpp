#pragma once

#include <hog/client/held_pieces.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/view.hpp>
#include <hog/protocol/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace hog::client {

// A node to ask the server for, the depth to have it down to, and how big it looks from the eye:
// its size over its distance, the finest size added so the node holding the eye has one, less
// where it lies behind where the eye looks.
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
// where the finest is wanted. Regions never overlap. Biggest looking first, `most` at most.
[[nodiscard]] std::vector<PieceWish> pieces_to_ask(const HeldPieces& held, DVec3 eye,
                                                   std::optional<DVec3> facing,
                                                   const grid::View& view, std::size_t most);

// The wishes as asked for over the wire.
[[nodiscard]] protocol::AskPieces ask_of(const std::vector<PieceWish>& wishes);

} // namespace hog::client
