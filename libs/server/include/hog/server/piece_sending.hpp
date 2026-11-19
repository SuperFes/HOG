#pragma once

#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/water_bodies.hpp>
#include <hog/grid/z_order.hpp>
#include <hog/net/events.hpp>
#include <hog/server/piece_source.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <vector>

namespace hog::server {

// The pieces each player asks for, sent in its order as deep as the source holds them, and again
// as more is filled under them or what they were read from changes, carrying only the kinds that
// changed. A piece the source can't give whole yet waits until something is written under it,
// rather than being read again each time.
class PieceSending {
public:
    // `finest`: the grid's deepest depth.
    explicit PieceSending(std::uint8_t finest) noexcept : finest_{finest} {}

    void join(net::ConnectionId player);
    void leave(net::ConnectionId player);

    // A node, the depth it's wanted down to, and how big it looks from the player, which filling
    // it is ranked by.
    struct Want {
        grid::Key node;
        std::uint8_t depth = 0;
        double priority = 0.0;
    };
    // Has the player sent the pieces it asks for, in its order, in place of whatever it was due
    // before, where it hasn't been sent them as deep. Nodes outside the grid are left out; depths
    // past the finest are taken as the finest, those over a node as the node's.
    void ask(net::ConnectionId player, std::span<const Want> wants);

    // Has the players holding anything touched sent what's changed of it, first, and those
    // waiting on anything touched due again in their place.
    void changed(std::span<const PieceSource::Touched> touched);

    // A piece as sent: its node, its bytes as `grid::Piece::encode` gives them, and the water
    // bodies its water tree marks that the player hasn't been told of, to tell it first.
    struct Sent {
        grid::Key node;
        std::vector<std::byte> bytes;
        std::vector<grid::Water::Value> bodies;
    };
    // The next pieces due to the player that `source` holds, in order, while they come to under
    // `room` bytes, one at least if any can go, `most` at most. One held as deep goes only with
    // the kinds changed since, and not at all where none has; one held deeper doesn't go.
    [[nodiscard]] std::vector<Sent>
    next(net::ConnectionId player, std::size_t room, PieceSource& source,
         std::size_t most = std::numeric_limits<std::size_t>::max());
    // Pieces still due to the player.
    [[nodiscard]] std::size_t due(net::ConnectionId player) const;
    // Pieces asked for that the source couldn't give whole, waiting until more is written.
    [[nodiscard]] std::size_t waiting(net::ConnectionId player) const;

private:
    // What a player holds under a node: how deep it was sent, what it was read from, whether it
    // went whole, and how big it looked when asked for.
    struct Holding {
        std::uint8_t depth = 0;
        PieceAnswer::Stamps stamps;
        bool whole = false;
        double priority = 0.0;
    };
    struct Player {
        grid::ZMap<Holding> held;
        std::vector<Want> due; // in order
        grid::ZMap<Want> waiting;
        std::set<grid::Water::Value> told;
        std::uint64_t round = 0; // when last asked
    };

    // Puts `want` among those due in order of how big it looks, unless its node is due already.
    static void due_again(Player& player, const Want& want);

    std::uint8_t finest_;
    std::map<net::ConnectionId, Player> players_;
    std::uint64_t rounds_ = 0;
};

// The bodies of `table` by `ids`, in messages' bytes as `grid::WaterBodies::encode` gives them,
// each `most` bytes at most; ids the table lacks, and a body too big alone, are left out.
[[nodiscard]] std::vector<std::vector<std::byte>>
bodies_coded(const grid::WaterBodies& table, std::span<const grid::Water::Value> ids,
             std::size_t most);

} // namespace hog::server
