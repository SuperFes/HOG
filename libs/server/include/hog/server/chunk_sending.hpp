#pragma once

#include <hog/baked/chunk_filling.hpp>
#include <hog/baked/chunk_store.hpp>
#include <hog/baked/filling.hpp>
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
#include <vector>

namespace hog::server {

// The store's chunks each player asks for, sent as they're stored, never decoded: for each node
// asked, in the player's order, every band's chunks over it or under it down to the depth asked.
// A chunk goes once at each version it's held at, and one the store no longer holds is told let
// go of. A node the store doesn't hold all of yet is handed on to be filled, and gone over again
// when something's written under it; so is one sent whole, so the player hears what changed.
class ChunkSending {
public:
    explicit ChunkSending(const baked::Bands& bands) noexcept : bands_{bands} {}

    void join(net::ConnectionId player);
    void leave(net::ConnectionId player);

    // A node, the depth it's wanted down to, and how big it looks from the player, which filling
    // it is ranked by.
    struct Want {
        grid::Key node;
        std::uint8_t depth = 0;
        double priority = 0.0;
    };
    // Has the player sent the nodes it asks for, in its order, in place of whatever it was due
    // before, where it hasn't been sent them whole as deep. Nodes outside the grid are left out;
    // depths past the finest are taken as the finest, those over a node as the node's.
    void ask(net::ConnectionId player, std::span<const Want> wants);

    // Has the nodes sent or waiting over what's written gone over again, in order of how big
    // they look.
    void changed(std::span<const baked::Region> written);
    // Takes the nodes the filler found nothing more to fill under as whole.
    void answered(std::span<const baked::ChunkFiller::Asked> asked);

    // A node the store held only part of, to be filled at its rank.
    struct Short {
        Want want;
        baked::Rank rank;
    };
    struct Next {
        std::vector<protocol::SentChunk> chunks;
        std::vector<Short> short_of;
        // Chunks held but too big to send.
        std::size_t too_big = 0;
    };
    // The chunks next due to the player as `snapshot` holds them, in order, while their trees
    // come to under `room` bytes, one at least if any is due, `most` at most.
    [[nodiscard]] Next next(net::ConnectionId player, const baked::ChunkStore::Snapshot& snapshot,
                            std::size_t room, std::size_t most);

    // Nodes still due to the player.
    [[nodiscard]] std::size_t due(net::ConnectionId player) const;
    // Nodes sent short, waiting on something written under them.
    [[nodiscard]] std::size_t waiting(net::ConnectionId player) const;

private:
    // A chunk as the player was sent it: kind, band, then its root as the store orders them.
    struct SentKey {
        std::uint8_t kind = 0;
        std::uint8_t band = 0;
        std::uint64_t code = 0; // the root's aligned code
        std::uint8_t depth = 0;

        friend constexpr auto operator<=>(const SentKey&, const SentKey&) noexcept = default;
    };
    struct Player {
        std::vector<Want> due; // in order
        grid::ZMap<Want> whole;
        grid::ZMap<Want> waiting;
        std::map<SentKey, std::uint64_t> sent; // by version
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
    // Into `next`, the band's chunks of `Kind` about `node` that the player hasn't as they're
    // held, and those it was sent there that are held no more, while they fit; whether they hold
    // all of `node`. Cut short, `limits.cut` says so.
    template <typename Kind>
    bool send_band(Player& player, const baked::ChunkStore::Snapshot& snapshot, std::uint8_t band,
                   grid::Key node, Limits& limits, Next& next) const;
    // Puts `want` among those due in order of how big it looks, unless its node is due already.
    static void due_again(Player& player, const Want& want);

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
