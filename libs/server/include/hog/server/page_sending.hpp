#pragma once

#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/view.hpp>
#include <hog/grid/water_bodies.hpp>
#include <hog/net/events.hpp>
#include <hog/server/page_source.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace hog::server {

// The grid pages each player is sent: the top page, and under each open leaf of a page sent whole
// the pages there, wherever the page's finest nodes look coarser than they should from the
// player. Each goes as deep as its nodes should be seen from there, deeper as the player nears.
// The page that looks biggest from the player goes first, so near detail comes before far, and a
// page always before those under it. A page the source can't give yet is asked for once and waits
// until it's written, rather than being read again each time.
class PageSending {
public:
    explicit PageSending(const grid::Grid& grid, const grid::View& view = grid::View{});

    void join(net::ConnectionId player);
    void leave(net::ConnectionId player);

    // Whether the player, where it is from the planet's centre, is over another finest page than
    // when last followed.
    [[nodiscard]] bool moved(net::ConnectionId player, DVec3 at) const;

    // A page, the levels of it wanted, and how big it looks: its size over its distance, the
    // finest size added so the page holding the player has one too.
    struct Want {
        grid::Key page;
        std::uint8_t levels = 0;
        double priority = 0.0;
    };
    // The pages standing for the world as seen from `at`, in the order they're sent, as far down
    // as `source` holds them; those it lacks are asked of its filler on the way, in `round`.
    [[nodiscard]] std::vector<Want> wanted(DVec3 at, PageSource& source,
                                           std::uint64_t round = 0) const;

    // Has the player sent the pages wanted from `at` it hasn't been sent as deep yet, in order.
    void follow(net::ConnectionId player, DVec3 at, PageSource& source);

    // Has the player sent the pages it asks for, in its order, as deep as asked where it hasn't
    // been sent them as deep, in place of whatever it was due before; from now on it's sent only
    // what it asks for, never followed. Pages that aren't pages' roots in the grid are left out,
    // and levels past a page's foot are cut to it.
    void ask(net::ConnectionId player, std::span<const Want> wants);
    // Whether the player asks for what it's sent.
    [[nodiscard]] bool asking(net::ConnectionId player) const;

    // Has the players holding any of `pages` sent what's changed of them, first, and those
    // waiting on any of them due again in their place.
    void changed(std::span<const grid::Key> pages);

    // A page as sent: its root and its bytes as `grid::Page::encode` gives them, holding only the
    // trees the player didn't hold as they are now; and the water bodies its water tree marks that
    // the player hasn't been told of, to tell it first.
    struct Sent {
        grid::Key root;
        std::vector<std::byte> bytes;
        std::vector<grid::Water::Value> bodies;
    };
    // The next pages due to the player that `source` holds deeper than sent, or newer, in order,
    // while they come to under `room` bytes, and one at least if any can go, `most` at most. Those
    // sent as deep as wanted aren't due again.
    [[nodiscard]] std::vector<Sent>
    next(net::ConnectionId player, std::size_t room, PageSource& source,
         std::size_t most = std::numeric_limits<std::size_t>::max());
    // Pages still due to the player, held or not.
    [[nodiscard]] std::size_t due(net::ConnectionId player) const;
    // Pages wanted that the source couldn't give, or not as deep, waiting until they're written.
    [[nodiscard]] std::size_t waiting(net::ConnectionId player) const;

private:
    // What a player holds of a page: how deep, and each tree's version.
    struct Holding {
        std::uint8_t levels = 0;
        std::uint32_t shape = 0;
        std::uint32_t material = 0;
        std::uint32_t water = 0;
        std::uint32_t woods = 0;
    };
    struct Player {
        std::optional<grid::Key> over; // the finest page it was last followed over
        std::map<grid::Key, Holding> sent;
        std::vector<Want> due; // in order
        std::map<grid::Key, Want> waiting;
        std::set<grid::Water::Value> told;
        std::uint64_t round = 0; // when last followed or asked
        bool asking = false;
    };

    [[nodiscard]] grid::Key over(DVec3 at) const;

    // Of the bodies `marked`, those `told` hadn't, now in it.
    [[nodiscard]] static std::vector<grid::Water::Value>
    newly_told(std::set<grid::Water::Value>& told, std::span<const grid::Water::Value> marked);
    // Has `holding` hold what's sent of `page`, `levels` deep.
    static void hold(Holding& holding, const grid::Page& page, std::uint8_t levels);

    grid::Grid grid_;
    grid::View view_;
    std::map<net::ConnectionId, Player> players_;
    std::uint64_t rounds_ = 0;
};

} // namespace hog::server
