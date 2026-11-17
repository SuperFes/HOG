#pragma once

#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/net/events.hpp>
#include <hog/server/page_source.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace hog::server {

// The grid pages each player is sent: the top page, and under each open leaf of a page sent whole
// the pages there, wherever the page's finest nodes look coarser than they should from the
// player. Each goes as deep as its nodes should be seen from there, deeper as the player nears.
// Coarse pages go first, then finer ones, nearest first within a depth.
class PageSending {
public:
    // Pixels a node may span before the page under it is wanted, in a picture 900 texels high
    // seen through a vertical field of view of one radian, as the client draws it.
    static constexpr double node_pixels = 8.0;
    static constexpr double view_height = 900.0;
    static constexpr double view_fov = 1.0;

    explicit PageSending(const grid::Grid& grid, double pixels = node_pixels);

    void join(net::ConnectionId player);
    void leave(net::ConnectionId player);

    // Whether the player, where it is from the planet's centre, is over another finest page than
    // when last followed.
    [[nodiscard]] bool moved(net::ConnectionId player, DVec3 at) const;

    // A page and the levels of it wanted.
    struct Want {
        grid::Key page;
        std::uint8_t levels = 0;

        friend constexpr bool operator==(const Want&, const Want&) noexcept = default;
    };
    // The pages standing for the world as seen from `at`, in the order they're sent, as far down
    // as `source` holds them; those it lacks are asked of its filler on the way.
    [[nodiscard]] std::vector<Want> wanted(DVec3 at, PageSource& source) const;
    // Levels of the page rooted at `root` wanted from `metres` away: down to nodes spanning the
    // pixels allowed, but at least one.
    [[nodiscard]] std::uint8_t levels_at(grid::Key root, double metres) const noexcept;

    // Has the player sent the pages wanted from `at` it hasn't been sent as deep yet, in order.
    void follow(net::ConnectionId player, DVec3 at, PageSource& source);

    // A page as sent: its root and its bytes as `grid::Page::encode` gives them.
    struct Sent {
        grid::Key root;
        std::vector<std::byte> bytes;
    };
    // The next pages due to the player that `source` holds deeper than sent, in order, while they
    // come to under `room` bytes, and one at least if any can go. Those sent as deep as wanted
    // aren't due again.
    [[nodiscard]] std::vector<Sent> next(net::ConnectionId player, std::size_t room,
                                         PageSource& source);
    // Pages still due to the player, held or not.
    [[nodiscard]] std::size_t due(net::ConnectionId player) const;

private:
    struct Player {
        std::optional<grid::Key> over;          // the finest page it was last followed over
        std::map<grid::Key, std::uint8_t> sent; // levels of each
        std::vector<Want> due;                  // in order
    };

    [[nodiscard]] grid::Key over(DVec3 at) const;
    // Whether the page at `foot` should be sent, its nodes as the page over it ends seen from
    // `metres` away.
    [[nodiscard]] bool refined(std::uint8_t foot, double metres) const noexcept;

    grid::Grid grid_;
    double angle_; // radians a node may span
    std::map<net::ConnectionId, Player> players_;
};

} // namespace hog::server
