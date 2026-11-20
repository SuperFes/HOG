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
#include <map>
#include <span>
#include <string>
#include <vector>

namespace hog::server {

// The store's chunks each player asks for, sent as they're stored, never decoded: for each node
// asked, in the player's order, every band's chunks over it or under it down to the depth asked.
// Nothing is kept of what a player holds: what it asks for is what it lacks, so each ask is
// answered from the store afresh, a chunk going once an ask. A node the store doesn't hold all of
// yet is handed on to be filled, and gone over again when something's written under it.
class ChunkSending {
public:
    explicit ChunkSending(const baked::Bands& bands) noexcept : bands_{bands} {}

    void join(net::ConnectionId player);
    void leave(net::ConnectionId player);

    // A node, the depth it's wanted down to, and how much it's wanted, which filling it is
    // ranked by.
    struct Want {
        grid::Key node;
        std::uint8_t depth = 0;
        double priority = 0.0;
    };
    // Has the player sent the nodes it asks for, in its order, in place of whatever it asked
    // before. Nodes outside the grid are left out; depths past the finest are taken as the
    // finest, those over a node as the node's.
    void ask(net::ConnectionId player, std::span<const Want> wants);

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
        // Chunks held but too big to send.
        std::size_t too_big = 0;
    };
    // The chunks next due to the player as `snapshot` holds them, in order, while their trees
    // come to under `room` bytes, one at least if any is due, `most` at most.
    [[nodiscard]] Next next(net::ConnectionId player, const baked::ChunkStore::Snapshot& snapshot,
                            std::size_t room, std::size_t most);

    // Nodes still to go over for the player.
    [[nodiscard]] std::size_t due(net::ConnectionId player) const;
    // Nodes gone over short, waiting on something written under them.
    [[nodiscard]] std::size_t waiting(net::ConnectionId player) const;
    // All that's kept of the player, for people to read: its ask, how far it's been gone over,
    // and what waits on fills or is known filled, each with whether `snapshot` holds it in each
    // band asked.
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
    struct Player {
        std::vector<Want> asked; // the last ask, in order
        std::size_t gone = 0;    // how many of it have been gone over
        std::vector<Want> again; // to go over once more, ahead of the rest
        grid::ZMap<Want> waiting;
        grid::ZMap<Want> answered;             // nothing more to fill under
        std::map<SentKey, std::uint64_t> sent; // this ask, by version
        std::uint64_t round = 0;               // when last asked
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
    // Into `next`, the band's chunks of `Kind` about `node` not sent this ask as they're held,
    // while they fit; whether they hold all of `node`. Cut short, `limits.cut` says so.
    template <typename Kind>
    bool send_band(Player& player, const baked::ChunkStore::Snapshot& snapshot, std::uint8_t band,
                   grid::Key node, Limits& limits, Next& next) const;
    // Goes over `want`: whether it was cut short for want of room.
    bool go_over(Player& player, const baked::ChunkStore::Snapshot& snapshot, const Want& want,
                 Limits& limits, Next& next) const;

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
