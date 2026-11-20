#pragma once

#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/baked/filling.hpp>
#include <hog/baked/wants_table.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>
#include <hog/grid/water_bodies.hpp>
#include <hog/grid/z_order.hpp>
#include <hog/net/events.hpp>
#include <hog/protocol/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hog::server {

// The store's chunks each player asks for, sent as they're stored, never decoded: for each node
// asked, in the player's order, every band's chunks over it or under it from past what it holds
// down to the depth asked, then word that it's done. Nothing is kept of what a player holds, only
// what it's asked for and not yet been told is done: each ask is answered from the store afresh,
// a chunk going once for the nodes asked together. A node the store doesn't hold all of yet is
// handed on to be filled, and gone over again when something's written under it.
class ChunkSending {
public:
    explicit ChunkSending(const baked::Bands& bands) noexcept : bands_{bands} {}

    void join(net::ConnectionId player);
    void leave(net::ConnectionId player);

    // A node, the depth it's wanted down to, the depth the player holds all of it down to, and
    // how much it's wanted, which filling it is ranked by.
    struct Want {
        grid::Key node;
        std::uint8_t depth = 0;
        std::uint8_t held = 0;
        double priority = 0.0;
    };
    // Has the player sent the nodes it asks for, in its order, after those it asked before, each
    // in place of one asked of the same node. Nodes outside the grid are left out; depths past
    // the finest are taken as the finest, those over a node as the node's.
    void ask(net::ConnectionId player, std::span<const Want> wants);
    // Has the player want the nodes no more, whether they're due or wait on fills.
    void drop(net::ConnectionId player, std::span<const grid::Key> nodes);

    // Has the nodes waiting on what's written gone over again.
    void changed(std::span<const baked::Region> written);
    // Has everything each player asked gone over again, as where what changed is unknown.
    void changed_everything();
    // Has the nodes the filler found nothing more to fill under gone over once more for what's
    // stored of them, and waited on no more however little that is.
    void answered(std::span<const baked::ChunkFiller::Asked> asked);

    // What every player waits on the store to hold more of, each node and depth once, at the
    // highest rank any asks it: what's to be filled.
    [[nodiscard]] std::vector<baked::WantsTable::Want> wanted() const;

    struct Next {
        std::vector<protocol::SentChunk> chunks;
        // The nodes these chunks finish, at most protocol::most_asked.
        std::vector<protocol::DonePiece> done;
        // Chunks held but too big to send.
        std::size_t too_big = 0;
    };
    // The chunks next due to the player as `snapshot` holds them, in order, while their trees
    // come to under `room` bytes, one at least if any is due, `most` at most; and the nodes
    // they finish. A node is told done not whole once, when gone over short, and whole once it's
    // gone over whole or the filler finds nothing more to fill under it.
    [[nodiscard]] Next next(net::ConnectionId player, const baked::ChunkStore::Snapshot& snapshot,
                            std::size_t room, std::size_t most);

    // Nodes still to go over for the player.
    [[nodiscard]] std::size_t due(net::ConnectionId player) const;
    // Nodes gone over short, waiting on something written under them.
    [[nodiscard]] std::size_t waiting(net::ConnectionId player) const;
    // All that's kept of the player, for people to read: what's due and what waits on fills,
    // each with whether `snapshot` holds it in each band asked.
    [[nodiscard]] std::string describe(net::ConnectionId player,
                                       const baked::ChunkStore::Snapshot& snapshot) const;

private:
    // A chunk as it was sent: kind, band, then its root as the store orders them.
    struct SentKey {
        std::uint8_t kind = 0;
        std::uint8_t band = 0;
        std::uint64_t code = 0; // the root's aligned code
        std::uint8_t depth = 0;

        friend constexpr auto operator<=>(const SentKey&, const SentKey&) noexcept = default;
    };
    using Sent = std::map<SentKey, std::uint64_t>; // by version
    // A node asked and not yet told done whole, with the ask it came in.
    struct Open {
        Want want;
        std::uint64_t ask = 0;
        bool answered = false; // nothing more to fill under it
        bool told = false;     // told done, not whole
    };
    // What's been sent of each ask while any node of it is open, and how many are.
    struct Asked {
        Sent sent;
        std::size_t open = 0;
    };
    struct Player {
        std::deque<Open> again; // to go over once more, ahead of the rest
        std::deque<Open> due;   // in the order asked
        grid::ZMap<Open> waiting;
        std::map<std::uint64_t, Asked> asks;
        std::uint64_t asked = 0; // asks so far
        std::uint64_t round = 0; // when last asked
    };

    // How much more a message may carry, and whether something due was left out for want of
    // room.
    struct Limits {
        std::size_t room = 0;
        std::size_t most = 0;
        std::size_t bytes = 0;
        bool cut = false;

        // Whether a chunk of `tree` bytes more fits: the first always does.
        [[nodiscard]] bool fits(const Next& next, std::size_t tree) const noexcept {
            return next.chunks.empty() || (next.chunks.size() < most && bytes + tree <= room);
        }
    };
    // Into `next`, the band's chunks of `Kind` about `node` not in `sent` as they're held,
    // while they fit; whether they hold all of `node`. Cut short, `limits.cut` says so.
    template <typename Kind>
    static bool send_band(Sent& sent, const baked::ChunkStore::Snapshot& snapshot,
                          std::uint8_t band, grid::Key node, Limits& limits, Next& next);
    // Goes over `open`: whether it was cut short for want of room; if not, whether it's still
    // open, waiting on fills.
    enum class Over : std::uint8_t { Cut, Done, Waiting };
    Over go_over(Player& player, const baked::ChunkStore::Snapshot& snapshot, Open& open,
                 Limits& limits, Next& next) const;
    // Takes the node's open want out of the player, as it was; none where none is open.
    static std::optional<Open> take_open(Player& player, grid::Key node);
    // Lets go of what's been sent for an ask, once none of it is open.
    static void close(Player& player, std::uint64_t ask);

    baked::Bands bands_;
    std::map<net::ConnectionId, Player> players_;
    std::uint64_t rounds_ = 0;
};

// The bodies of `table` by `ids`, in messages' bytes as `grid::WaterBodies::encode` gives them,
// each `most` bytes at most; ids the table lacks, and a body too big alone, are left out.
[[nodiscard]] std::vector<std::vector<std::byte>>
bodies_coded(const grid::WaterBodies& table, std::span<const grid::Water::Value> ids,
             std::size_t most);

} // namespace hog::server
