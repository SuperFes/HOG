#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/land.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/volume.hpp>
#include <hog/net/events.hpp>
#include <hog/protocol/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace hog::server {

// The tiles each player is sent: the land cut finer the nearer the player, nearest first. A
// player holds a tile once sent it, until it's cut otherwise.
class TileSending {
public:
    // Pixels a voxel spans where its tiles are coarsest for their distance, in a picture 900 texels
    // high seen through a vertical field of view of one radian, as the client draws it. A voxel
    // spans this to twice it everywhere.
    static constexpr double voxel_pixels = 8.0;
    static constexpr double view_height = 900.0;
    static constexpr double view_fov = 1.0;

    // Metres from a player within which its tiles are the finest, for voxels `voxel` metres
    // across to span `pixels` at most.
    [[nodiscard]] static double reach_for(double voxel, double pixels = voxel_pixels) noexcept;

    // Tiles are the finest within `reach` metres of a player; a level coarser for each doubling
    // past it.
    explicit TileSending(double reach) : reach_{reach} {}

    void join(net::ConnectionId player);
    void leave(net::ConnectionId player);

    // Whether the player, where it is from the planet's centre, is over another finest tile than
    // when last cut for, which it's now taken to be over.
    [[nodiscard]] bool moved(net::ConnectionId player, DVec3 at, const globe::Layers& layers);
    // The tiles held by the player now, to cut for it away from here.
    [[nodiscard]] std::set<globe::TileKey> held(net::ConnectionId player) const;

    // The land cut for a player: the tiles standing for it, and those of them not held, coded.
    struct Cut {
        std::vector<globe::TileKey> keys;
        std::vector<protocol::SentTile> sent; // nearest first
        std::size_t too_big = 0;              // over protocol::most_tile, left out
    };
    // The land cut as seen from `at`, those already `held` not coded again; making what it must,
    // spread by `each`. Touches nothing of this.
    [[nodiscard]] Cut cut(DVec3 at, const std::set<globe::TileKey>& held, globe::Land& land,
                          const globe::EachOf& each = {}) const;
    // Has the player sent the cut's tiles from now on, letting go of those it held not in it.
    void take(net::ConnectionId player, Cut cut);
    // moved(), cut() and take() all at once.
    void follow(net::ConnectionId player, DVec3 at, globe::Land& land,
                const globe::EachOf& each = {});

    // The next tiles for the player, nearest first, while they come to under `room` bytes, and one
    // at least if any is due. Those sent count as held.
    [[nodiscard]] protocol::Tiles next(net::ConnectionId player, std::size_t room);
    // Tiles still due to the player.
    [[nodiscard]] std::size_t due(net::ConnectionId player) const;

    // The tiles standing for the land as seen from `at`, nearest first, each place in the shell in
    // one of them: each at the level for its distance, or coarser where nothing of it shows from
    // outside, as all one voxel or buried.
    [[nodiscard]] std::vector<globe::TileKey> wanted(DVec3 at, globe::Land& land,
                                                     const globe::EachOf& each = {}) const;
    // The level a tile is seen at from `metres` away, no coarser than `top`.
    [[nodiscard]] std::uint8_t level_at(double metres, std::uint8_t top) const noexcept;

private:
    struct Player {
        std::optional<globe::TileKey> over; // the finest tile it was last in
        std::set<globe::TileKey> held;
        std::vector<protocol::SentTile> due; // nearest first
    };

    double reach_;
    std::map<net::ConnectionId, Player> players_;
};

// Metres from `at` to the nearest of the tile at `key`, near enough: its cells' corners bound it
// across, its layers up.
[[nodiscard]] double metres_to(const globe::Layers& layers, globe::TileKey key, DVec3 at);

} // namespace hog::server
